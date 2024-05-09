#include <execution>
#include "process_queries.h"
std::vector<std::vector<Document>> ProcessQueries(
    const SearchServer& search_server,
    const std::vector<std::string>& queries)
{
    std::vector<std::vector<Document>> documents_lists(queries.size());
    transform(std::execution::par, queries.begin(), queries.end(), documents_lists.begin(),
              [&](const auto& query) { return search_server.FindTopDocuments(query); });
    return documents_lists;
}

std::list<Document> ProcessQueriesJoined(
    const SearchServer& search_server,
    const std::vector<std::string>& queries)
{
    std::list<Document> r;
    auto ans=ProcessQueries(search_server, queries);
    for(const auto& item : ans)
    {
        std::move(item.begin(), item.end(), std::back_inserter(r));
    }
    return r;
}