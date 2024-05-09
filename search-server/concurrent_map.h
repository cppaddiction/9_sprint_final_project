#include <map>
#include <mutex>
#include <vector>

using namespace std::string_literals;

template <typename Key, typename Value>
class ConcurrentMap {
public:
    static_assert(std::is_integral_v<Key>, "ConcurrentMap supports only integer keys"s);
    
    explicit ConcurrentMap(size_t bucket_count) : megamap_(bucket_count) {}
 
    struct Access {
        Access(const Key& key, std::map<Key, Value>& localmap, std::mutex& m) : lock(m), ref_to_value(localmap[key]) {}
        ~Access() {}
        std::lock_guard<std::mutex> lock;
        Value& ref_to_value;
    };
    
    struct Bucket {
        std::map<Key, Value> local;
        std::mutex localm;
    };
 
    Access operator[](const Key& key)
    {
        auto select = static_cast<uint64_t>(key) % megamap_.size();
        return Access(key, (megamap_[select]).local, (megamap_[select]).localm);
    }
 
    std::map<Key, Value> BuildOrdinaryMap()
    {
        std::map<Key, Value> total;
        for(auto& item : megamap_)
        {
            std::lock_guard guard(item.localm);
            total.insert(item.local.begin(), item.local.end());
        }
        return total;
    }
    
    void Erase(const Key& key)
    {
        for(auto& item : megamap_)
        {
            if(item.local.find(key)!=item.local.end())
            {
                std::lock_guard guard(item.localm);
                item.local.erase(key);
            }
        }
    }
private:
    std::vector<Bucket> megamap_;
};