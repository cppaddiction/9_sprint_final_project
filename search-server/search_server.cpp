#include "search_server.h"

int SearchServer::GetDocumentCount() const {
    return documents_.size();
}

bool SearchServer::IsStopWord(const std::string& word) const {
    return stop_words_.count(word) > 0;
}

std::vector<std::string> SearchServer::SplitIntoWordsNoStop(const std::string& text) const {
    std::vector<std::string> words;
    for (const std::string& word : SplitIntoWords(text)) {
        if (!IsValidWord(word)) {
            using namespace std::string_literals;
            throw std::invalid_argument("Word "s + word + " is invalid"s);
        }
        if (!IsStopWord(word)) {
            words.push_back(word);
        }
    }
    return words;
}

SearchServer::QueryWord SearchServer::ParseQueryWord(const std::string& text) const {
    if (text.empty()) {
        using namespace std::string_literals;
        throw std::invalid_argument("Query word is empty"s);
    }
    std::string word = text;
    bool is_minus = false;
    if (word[0] == '-') {
        is_minus = true;
        word = word.substr(1);
    }
    if (word.empty() || word[0] == '-' || !IsValidWord(word)) {
        using namespace std::string_literals;
        throw std::invalid_argument("Query word "s + text + " is invalid");
    }
    return {word, is_minus, IsStopWord(word)};
}

SearchServer::Query SearchServer::ParseQuery(const std::string& text, bool is_parallel) const {
    SearchServer::Query result;
    for (const std::string& word : SplitIntoWords(text)) {
        const auto& query_word = ParseQueryWord(word);
        if (!query_word.is_stop) {
            if (query_word.is_minus) {
                result.minus_words.push_back(query_word.data);
            } else {
                result.plus_words.push_back(query_word.data);
            }
        }
    }
    if(is_parallel)
    {
        std::sort(result.plus_words.begin(), result.plus_words.end());
        std::sort(result.minus_words.begin(), result.minus_words.end());
        result.plus_words.erase(std::unique(result.plus_words.begin(), result.plus_words.end()), result.plus_words.end());
        result.minus_words.erase(std::unique(result.minus_words.begin(), result.minus_words.end()), result.minus_words.end());
    }
    return result;
}

double SearchServer::ComputeWordInverseDocumentFreq(const std::string& word) const {
    return log(GetDocumentCount() * 1.0 / word_to_document_freqs_.at(word).size());
}

void AddDocument(SearchServer& search_server, int document_id, const std::string& document,
                 DocumentStatus status, const std::vector<int>& ratings) {
    using namespace std::string_literals;
    try {
        search_server.AddDocument(document_id, document, status, ratings);
    } catch (const std::exception& e) {
        std::cout << "Error in adding document "s << document_id << ": "s << e.what() << std::endl;
    }
}
 
void FindTopDocuments(const SearchServer& search_server, const std::string& raw_query) {
    using namespace std::string_literals;
    std::cout << "Results for request: "s << raw_query << std::endl;
    try {
        for (const Document& document : search_server.FindTopDocuments(raw_query)) {
            PrintDocument(document);
        }
    } catch (const std::exception& e) {
        std::cout << "Error is seaching: "s << e.what() << std::endl;
    }
}

bool SearchServer::IsValidWord(const std::string& word) {
    return none_of(word.begin(), word.end(), [](char c) {
        return c >= '\0' && c < ' ';
    });
}

int SearchServer::ComputeAverageRating(const std::vector<int>& ratings) {
    int rating_sum = 0;
    for (const int rating : ratings) {
        rating_sum += rating;
    }
    return rating_sum / static_cast<int>(ratings.size());
}

std::set<int>::const_iterator SearchServer::begin() const
{
    return document_ids_.begin();
}

std::set<int>::const_iterator SearchServer::end() const
{
    return document_ids_.end();
}

const std::map<std::string_view, double>& SearchServer::GetWordFrequencies(int document_id) const
{
    static const std::map<std::string_view, double> emptymap;
    auto result=document_to_word_freqs_.find(document_id);
    if(result!=document_to_word_freqs_.end())
    {
        return result->second;
    }
    return emptymap;
}

void SearchServer::AddDocument(int document_id, std::string_view document, DocumentStatus status, const std::vector<int>& ratings) {
    if ((document_id < 0) || (documents_.count(document_id) > 0)) {
        using namespace std::string_literals;
        throw std::invalid_argument("Invalid document_id"s);
    }
    const auto words = SplitIntoWordsNoStop(static_cast<std::string>(document));
    const double inv_word_count = 1.0 / words.size();
    for (const std::string& word : words) {
        word_to_document_freqs_[word][document_id] += inv_word_count;
        document_to_word_freqs_[document_id][word] += inv_word_count;
    }
    documents_.emplace(document_id, DocumentData{ComputeAverageRating(ratings), status});
    document_ids_.insert(document_id);
}

void SearchServer::RemoveDocument(int document_id)
{
    const auto & word_frequencies=GetWordFrequencies(document_id);
    for(std::map<std::string_view, double>::const_iterator it = word_frequencies.begin(); it != word_frequencies.end(); ++it) {
        word_to_document_freqs_[static_cast<std::string>(it->first)].erase(document_id);
    }
    document_to_word_freqs_.erase(document_id);
    documents_.erase(document_id);
    document_ids_.erase(document_id);
}

void SearchServer::RemoveDocument(std::execution::sequenced_policy policy, int document_id)
{
    RemoveDocument(document_id);
}

void SearchServer::RemoveDocument(std::execution::parallel_policy, int document_id)
{
    const auto & word_frequencies=GetWordFrequencies(document_id);
    std::vector<std::string_view> result(word_frequencies.size());
    transform(std::execution::par, word_frequencies.begin(), word_frequencies.end(), result.begin(),
              [&](const auto& it){return it.first;});
    std::for_each(std::execution::par, result.begin(), result.end(), [&](const auto& p){word_to_document_freqs_[static_cast<std::string>(p)].erase(document_id);});    
    document_to_word_freqs_.erase(document_id);
    documents_.erase(document_id);
    document_ids_.erase(document_id);
}

matched_documents SearchServer::MatchDocument(std::string_view raw_query, int document_id) const {
    if(document_ids_.find(document_id) == document_ids_.end())
        throw std::out_of_range("Invalid document id");
    
    static const auto& query = ParseQuery(static_cast<std::string>(raw_query), true);
    std::vector<std::string_view> matched_words;
    
    for (const std::string& word : query.minus_words) {
        if (word_to_document_freqs_.count(word)!=0&&word_to_document_freqs_.at(word).count(document_id)) {
            return {matched_words, documents_.at(document_id).status};
        }
    }
    
    for (const std::string& word : query.plus_words) {
        if (word_to_document_freqs_.count(word)!=0&&word_to_document_freqs_.at(word).count(document_id)) {
            matched_words.push_back(std::string_view(word));
        }
    }
    
    return {matched_words, documents_.at(document_id).status};
}

matched_documents SearchServer::MatchDocument(std::execution::sequenced_policy policy, std::string_view raw_query,
                                                        int document_id) const
{
    return MatchDocument(raw_query, document_id);
}

matched_documents SearchServer::MatchDocument(std::execution::parallel_policy, std::string_view raw_query,
                                                        int document_id) const
{
    if(document_ids_.find(document_id) == document_ids_.end())
        throw std::out_of_range("Invalid document id");
    
    static const auto& query = ParseQuery(static_cast<std::string>(raw_query));    
    std::vector<std::string_view> matched_words;

    auto ans = std::find_if(std::execution::par, query.minus_words.begin(), query.minus_words.end(), [&](const auto& it){return word_to_document_freqs_.count(it)!=0&&word_to_document_freqs_.at(it).count(document_id) ? true : false;});
    
    if(ans!=query.minus_words.end())
    {
        return {matched_words, documents_.at(document_id).status};
    }
    else
    {
        matched_words.resize(query.plus_words.size());
        matched_words.resize(std::copy_if(std::execution::par, query.plus_words.begin(), query.plus_words.end(), matched_words.begin(), [&](const auto& it){return word_to_document_freqs_.count(it)!=0&&word_to_document_freqs_.at(it).count(document_id)?true:false;})-matched_words.begin());
        std::sort(matched_words.begin(), matched_words.end());
        matched_words.erase(std::unique(matched_words.begin(), matched_words.end()), matched_words.end());
        return {matched_words, documents_.at(document_id).status};
    }
}

std::vector<Document> SearchServer::FindTopDocuments(std::string_view raw_query, DocumentStatus status) const {
    return FindTopDocuments(std::execution::seq, raw_query, [status](int document_id, DocumentStatus document_status, int rating) {
            return document_status == status;
        });
}

std::vector<Document> SearchServer::FindTopDocuments(std::string_view raw_query) const
{
    return FindTopDocuments(raw_query, DocumentStatus::ACTUAL);
}