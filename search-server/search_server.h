#pragma once
#include "document.h"
#include "concurrent_map.h"
#include "string_processing.h"
#include <map>
#include <cmath>
#include <future>
#include <iterator>
#include <typeinfo>
#include <algorithm>
#include <execution>

const double EPSILON = 1e-6;
const int MAX_RESULT_DOCUMENT_COUNT = 5;
typedef std::tuple<std::vector<std::string_view>, DocumentStatus> matched_documents;

class SearchServer {
public:
    template <typename StringContainer>
    explicit SearchServer(const StringContainer& stop_words)
        : stop_words_(MakeUniqueNonEmptyStrings(stop_words))  
    {
        if (!all_of(stop_words_.begin(), stop_words_.end(), IsValidWord)) {
            using namespace std::string_literals;
            throw std::invalid_argument("Some of stop words are invalid"s);
        }
    }
    explicit SearchServer(const std::string& stop_words_text)
        : SearchServer(std::string_view(stop_words_text))
    {
    }
    explicit SearchServer(std::string_view stop_words_text)
        : SearchServer(SplitIntoWords(static_cast<std::string>(stop_words_text)))
    {
    }
    
    void AddDocument(int document_id, std::string_view document, DocumentStatus status,
                     const std::vector<int>& ratings);
    
    template <typename DocumentPredicate>
    std::vector<Document> FindTopDocuments(std::string_view raw_query,
                                      DocumentPredicate document_predicate) const;
    
    template <typename DocumentPredicate, typename ExecutionPolicy>
    std::vector<Document> FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query,
                                      DocumentPredicate document_predicate) const;
    
    std::vector<Document> FindTopDocuments(std::string_view raw_query, DocumentStatus status) const;
    
    template <typename ExecutionPolicy>
    std::vector<Document> FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query, DocumentStatus status) const;
    
    std::vector<Document> FindTopDocuments(std::string_view raw_query) const;
    
    template <typename ExecutionPolicy>
    std::vector<Document> FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query) const;

    int GetDocumentCount() const;
    
    std::set<int>::const_iterator begin() const;
    std::set<int>::const_iterator end() const;
    
    const std::map<std::string_view, double>& GetWordFrequencies(int document_id) const;
    
    void RemoveDocument(std::execution::sequenced_policy policy, int document_id);
    void RemoveDocument(std::execution::parallel_policy, int document_id);
    void RemoveDocument(int document_id);
    
    matched_documents MatchDocument(std::execution::sequenced_policy policy, std::string_view raw_query,
                                                        int document_id) const;
    matched_documents MatchDocument(std::execution::parallel_policy, std::string_view raw_query,
                                                        int document_id) const;
    matched_documents MatchDocument(std::string_view raw_query,
                                                        int document_id) const;
private:
    struct DocumentData {
        int rating;
        DocumentStatus status;
    };
    
    const std::set<std::string> stop_words_;
    std::map<std::string, std::map<int, double>> word_to_document_freqs_;
    std::map<int, std::map<std::string_view, double>> document_to_word_freqs_;
    std::map<int, DocumentData> documents_;
    std::set<int> document_ids_;
    bool IsStopWord(const std::string& word) const;
    static bool IsValidWord(const std::string& word);
    std::vector<std::string> SplitIntoWordsNoStop(const std::string& text) const;
    static int ComputeAverageRating(const std::vector<int>& ratings);
    
    struct QueryWord {
        std::string data;
        bool is_minus;
        bool is_stop;
    };
    
    QueryWord ParseQueryWord(const std::string& text) const;
    
    struct Query {
        std::vector<std::string> plus_words;
        std::vector<std::string> minus_words;
    };
    
    Query ParseQuery(const std::string& text, bool is_parallel=false) const;
    
    double ComputeWordInverseDocumentFreq(const std::string& word) const;
    
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(const Query& query,
                                           DocumentPredicate document_predicate) const;
    
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(std::execution::sequenced_policy, const Query& query,
                                           DocumentPredicate document_predicate) const;
    
    template <typename DocumentPredicate>
    std::vector<Document> FindAllDocuments(std::execution::parallel_policy, const Query& query,
                                           DocumentPredicate document_predicate) const;
};



template <typename DocumentPredicate>
    std::vector<Document> SearchServer::FindAllDocuments(std::execution::sequenced_policy, const Query& query,
                                           DocumentPredicate document_predicate) const
{
    std::map<int, double> document_to_relevance;
        for (const std::string& word : query.plus_words) {
            if (word_to_document_freqs_.count(word) != 0) {
                const double inverse_document_freq = ComputeWordInverseDocumentFreq(word);
                for (const auto [document_id, term_freq] : word_to_document_freqs_.at(word)) {
                    const auto& document_data = documents_.at(document_id);
                    if (document_predicate(document_id, document_data.status, document_data.rating)) {
                        document_to_relevance[document_id] += term_freq * inverse_document_freq;
                    }
                }
            }
        }
        for (const std::string& word : query.minus_words) {
            if (word_to_document_freqs_.count(word) != 0) {
                for (const auto [document_id, _] : word_to_document_freqs_.at(word)) {
                    document_to_relevance.erase(document_id);
                }
            }
        }
        std::vector<Document> matched_documents;
        for (const auto [document_id, relevance] : document_to_relevance) {
            matched_documents.push_back(
                {document_id, relevance, documents_.at(document_id).rating});
        }
        return matched_documents;
}

template <typename DocumentPredicate>
std::vector<Document> SearchServer::FindAllDocuments(std::execution::parallel_policy, const Query& query,
                                           DocumentPredicate document_predicate) const {
    const int buckets=100;
    ConcurrentMap<int, double> document_to_relevance(buckets);

    std::for_each(std::execution::par, query.plus_words.begin(), query.plus_words.end(), 
                  [&](const std::string& word)
                  {
                      if (word_to_document_freqs_.count(word) != 0) {
                          const double inverse_document_freq = ComputeWordInverseDocumentFreq(word);
                          for (const auto [document_id, term_freq] : word_to_document_freqs_.at(word)) {
                              const auto& document_data = documents_.at(document_id);
                              if (document_predicate(document_id, document_data.status, document_data.rating)) {
                                  document_to_relevance[document_id].ref_to_value += term_freq * inverse_document_freq;
                              }
                          }
                      }  
                  }
                 );

    std::for_each(std::execution::par, query.minus_words.begin(), query.minus_words.end(), 
                  [&](const std::string& word)
                  {
                      if (word_to_document_freqs_.count(word) != 0) {
                          for (const auto [document_id, _] : word_to_document_freqs_.at(word)) {
                              document_to_relevance.Erase(document_id);
                          }
                      }      
                  }
                 );

    std::vector<Document> matched_documents;
    const std::map<int, double>& result=document_to_relevance.BuildOrdinaryMap();

    for (const auto [document_id, relevance] : result) {
        matched_documents.push_back(
            {document_id, relevance, documents_.at(document_id).rating});
    }

    return matched_documents;
}

template <typename DocumentPredicate>
std::vector<Document> SearchServer::FindTopDocuments(std::string_view raw_query, DocumentPredicate document_predicate) const
{
    return FindTopDocuments(std::execution::seq, raw_query, document_predicate);
}

template <typename DocumentPredicate, typename ExecutionPolicy>
std::vector<Document> SearchServer::FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query,
                                                         DocumentPredicate document_predicate) const {
    const auto query = ParseQuery(static_cast<std::string>(raw_query), true);
    auto matched_documents = FindAllDocuments(policy, query, document_predicate);
    sort(policy, matched_documents.begin(), matched_documents.end(),
         [](const Document& lhs, const Document& rhs) {
             return lhs.relevance > rhs.relevance
                 || (std::abs(lhs.relevance - rhs.relevance) < EPSILON && lhs.rating > rhs.rating);
         });
    if (matched_documents.size() > MAX_RESULT_DOCUMENT_COUNT) {
        matched_documents.resize(MAX_RESULT_DOCUMENT_COUNT);
    }
    return matched_documents;
}

template <typename ExecutionPolicy>
std::vector<Document> SearchServer::FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query, DocumentStatus status) const
{
    return FindTopDocuments(policy, raw_query, [status](int document_id, DocumentStatus document_status, int rating) {
            return document_status == status;
        });
}

template <typename ExecutionPolicy>
std::vector<Document> SearchServer::FindTopDocuments(ExecutionPolicy&& policy, std::string_view raw_query) const
{
    return FindTopDocuments(policy, raw_query, DocumentStatus::ACTUAL);
}

void AddDocument(SearchServer& search_server, int document_id, const std::string& document,
                 DocumentStatus status, const std::vector<int>& ratings);
void FindTopDocuments(const SearchServer& search_server, const std::string& raw_query);
void MatchDocuments(const SearchServer& search_server, const std::string& query);