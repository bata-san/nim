// Standalone CPU benchmark for deferred material-job bookkeeping; no D3D12 needed.
#include "nim/render/material_job_index.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>
struct Old {
    // The former render path also inserted addresses into a map it did not read.
    std::unordered_map<uint64_t, size_t> map, unused_addresses;
    void record(uint64_t key, size_t address) { unused_addresses.emplace(key, address); }
    void clear(){ map.clear(); unused_addresses.clear(); }
    std::pair<size_t,bool> find_or_insert(uint64_t k,size_t n){
        const auto [p, inserted]=map.try_emplace(k,n);
        return {p->second,inserted};
    }
};
struct New : nim::MaterialJobIndex { void record(uint64_t, size_t) {} };
template<class T> std::pair<double,uint64_t> run(const std::vector<uint64_t>& keys) {
    T t;
    size_t jobs=0;
    uint64_t check=0;
    std::vector<uint64_t> owners;
    owners.reserve(keys.size());
    for (auto k : keys) jobs += t.find_or_insert(k,jobs).second;
    const auto start=std::chrono::steady_clock::now();
    for (int pass=0;pass<1200;++pass) {
        t.clear();
        jobs=0;
        owners.clear();
        for(auto k:keys){
            const auto [j,created]=t.find_or_insert(k,jobs);
            jobs += created;
            if (created) owners.push_back(k);
            check += j;
        }
        for (size_t j = 0; j < owners.size(); ++j) t.record(owners[j], (j + 1) * 256);
    }
    return {std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),check};
}
int main(){
    std::mt19937 rng(142);
    for (auto n : {300, 1400, 4200}) {
        std::vector<uint64_t> keys;
        for (unsigned i=0;i<8000;++i){
            const uint64_t k=rng()%n;
            keys.push_back(((k/173)<<33) | ((0x80A00000ull+(k%173)/2)<<1) | (k%173&1));
        }
        std::vector<double> old,newer;
        for(unsigned i=0;i<5;++i){
            std::pair<double,uint64_t> a,b;
            if(i%2){b=run<New>(keys);a=run<Old>(keys);}
            else {a=run<Old>(keys);b=run<New>(keys);}
            if(a.second!=b.second)return 1;
            old.push_back(a.first);
            newer.push_back(b.first);
        }
        std::sort(old.begin(),old.end());
        std::sort(newer.begin(),newer.end());
        std::printf("8,000 requests per pass, up to %d keys: %.2f -> %.2f ms (1,200 passes)\n",n,old[2],newer[2]);
    }
}
