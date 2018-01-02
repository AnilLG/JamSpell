#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>
#include <sstream>
#include <ostream>

#include "lang_model.hpp"

#include <contrib/cityhash/city.h>


namespace NOpenSpell {

template<typename T>
std::string DumpKey(const T& key) {
    std::stringbuf buf;
    std::ostream out(&buf);
    NSaveLoad::Save(out, key);
    return buf.str();
}

template<typename T>
void PrepareNgramKeys(const T& grams, std::vector<std::string>& keys) {
    for (auto&& it: grams) {
        keys.push_back(DumpKey(it.first));
    }
}

template<typename T>
void InitializeBuckets(const T& grams, TPerfectHash& ph, std::vector<std::pair<uint32_t, TCount>>& buckets) {
    for (auto&& it: grams) {
        std::string key = DumpKey(it.first);
        uint32_t bucket = ph.Hash(key);
        if (bucket >= buckets.size()) {
            std::cerr << bucket << " " << buckets.size() << "\n";
        }
        assert(bucket < buckets.size());
        std::pair<uint32_t, TCount> data;
        data.first = CityHash32(&key[0], key.size());
        data.second = it.second;
        buckets[bucket] = data;
    }
}

bool TLangModel::Train(const std::string& fileName, const std::string& alphabetFile) {
    std::cerr << "[info] loading text" << std::endl;
    if (!Tokenizer.LoadAlphabet(alphabetFile)) {
        std::cerr << "[error] failed to load alphabet" << std::endl;
        return false;
    }
    std::wstring trainText = UTF8ToWide(LoadFile(fileName));
    ToLower(trainText);
    TSentences sentences = Tokenizer.Process(trainText);
    if (sentences.empty()) {
        std::cerr << "[error] no sentences" << std::endl;
        return false;
    }

    TIdSentences sentenceIds = ConvertToIds(sentences);

    assert(sentences.size() == sentenceIds.size());
    {
        std::wstring tmp;
        trainText.swap(tmp);
    }
    {
        TSentences tmp;
        sentences.swap(tmp);
    }

    std::cerr << "[info] generating N-grams " << sentences.size() << std::endl;
    uint64_t lastTime = GetCurrentTimeMs();
    size_t total = sentenceIds.size();
    for (size_t i = 0; i < total; ++i) {
        const TWordIds& words = sentenceIds[i];

        for (auto w: words) {
            Grams1[w] += 1;
            TotalWords += 1;
        }

        for (ssize_t j = 0; j < (ssize_t)words.size() - 1; ++j) {
            TGram2Key key(words[j], words[j+1]);
            Grams2[key] += 1;
        }
        for (ssize_t j = 0; j < (ssize_t)words.size() - 2; ++j) {
            TGram3Key key(words[j], words[j+1], words[j+2]);
            Grams3[key] += 1;
        }
        uint64_t currTime = GetCurrentTimeMs();
        if (currTime - lastTime > 4000) {
            std::cerr << "[info] processed " << (100.0 * float(i) / float(total)) << "%" << std::endl;
            lastTime = currTime;
        }
    }

    std::cerr << "[info] generating keys" << std::endl;

    std::vector<std::string> keys;
    keys.reserve(Grams1.size() + Grams2.size() + Grams3.size());

    std::cerr << "[info] ngrams1: " << Grams1.size() << "\n";
    std::cerr << "[info] ngrams2: " << Grams2.size() << "\n";
    std::cerr << "[info] ngrams3: " << Grams3.size() << "\n";
    std::cerr << "[info] total: " << Grams3.size() + Grams2.size() + Grams1.size() << "\n";

    PrepareNgramKeys(Grams1, keys);
    PrepareNgramKeys(Grams2, keys);
    PrepareNgramKeys(Grams3, keys);

    std::cerr << "[info] generating perf hash" << std::endl;

    PerfectHash.Init(keys);

    std::cerr << "[info] finished, buckets: " << PerfectHash.BucketsNumber() << "\n";

    Buckets.resize(PerfectHash.BucketsNumber());
    InitializeBuckets(Grams1, PerfectHash, Buckets);
    InitializeBuckets(Grams2, PerfectHash, Buckets);
    InitializeBuckets(Grams3, PerfectHash, Buckets);

    std::cerr << "[info] buckets filled" << std::endl;

    return true;
}

double TLangModel::Score(const TWords& words) const {
    TWordIds sentence;
    for (auto&& w: words) {
        sentence.push_back(GetWordIdNoCreate(w));
    }
    if (sentence.empty()) {
        return std::numeric_limits<double>::min();
    }

    sentence.push_back(UnknownWordId);
    sentence.push_back(UnknownWordId);

    double result = 0;
    for (size_t i = 0; i < sentence.size() - 2; ++i) {
        result += log(GetGram1Prob(sentence[i]));
        result += log(GetGram2Prob(sentence[i], sentence[i + 1]));
        result += log(GetGram3Prob(sentence[i], sentence[i + 1], sentence[i + 2]));
    }
    return result;
}

double TLangModel::Score(const std::wstring& str) const {
    TSentences sentences = Tokenizer.Process(str);
    TWords words;
    for (auto&& s: sentences) {
        for (auto&& w: s) {
            words.push_back(w);
        }
    }
    return Score(words);
}

void TLangModel::Save(const std::string& modelFileName) const {
    std::ofstream out(modelFileName, std::ios::binary);
    NSaveLoad::Save(out, MAGIC_BYTE);
    NSaveLoad::Save(out, VERSION);
    Save(out);
    NSaveLoad::Save(out, MAGIC_BYTE);
}

bool TLangModel::Load(const std::string& modelFileName) {
    std::ifstream in(modelFileName, std::ios::binary);
    uint16_t version = 0;
    uint64_t magicByte = 0;
    NSaveLoad::Load(in, magicByte);
    if (magicByte != MAGIC_BYTE) {
        return false;
    }
    NSaveLoad::Load(in, version);
    if (version != VERSION) {
        return false;
    }
    Load(in);
    magicByte = 0;
    NSaveLoad::Load(in, magicByte);
    if (magicByte != MAGIC_BYTE) {
        Clear();
        return false;
    }
    IdToWord.clear();
    IdToWord.resize(WordToId.size() + 1, nullptr);
    for (auto&& it: WordToId) {
        IdToWord[it.second] = &it.first;
    }
    return true;
}

void TLangModel::Clear() {
    K = DEFAULT_K;
    WordToId.clear();
    LastWordID = 0;
    TotalWords = 0;
    Grams1.clear();
    Grams2.clear();
    Grams3.clear();
    Tokenizer.Clear();
}

const std::unordered_map<std::wstring, TWordId>& TLangModel::GetWordToId() {
    return WordToId;
}

TIdSentences TLangModel::ConvertToIds(const TSentences& sentences) {
    TIdSentences newSentences;
    for (size_t i = 0; i < sentences.size(); ++i) {
        const TWords& words = sentences[i];
        TWordIds wordIds;
        for (size_t j = 0; j < words.size(); ++j) {
            const TWord& word = words[j];
            wordIds.push_back(GetWordId(word));
        }
        newSentences.push_back(wordIds);
    }
    return newSentences;
}

TWordId TLangModel::GetWordId(const TWord& word) {
    assert(word.Ptr && word.Len);
    assert(word.Len < 10000);
    std::wstring w(word.Ptr, word.Len);
    auto it = WordToId.find(w);
    if (it != WordToId.end()) {
        return it->second;
    }
    TWordId wordId = LastWordID;
    ++LastWordID;
    it = WordToId.insert(std::make_pair(w, wordId)).first;
    IdToWord.push_back(&(it->first));
    return wordId;
}

TWordId TLangModel::GetWordIdNoCreate(const TWord& word) const {
    std::wstring w(word.Ptr, word.Len);
    auto it = WordToId.find(w);
    if (it != WordToId.end()) {
        return it->second;
    }
    return UnknownWordId;
}

TWord TLangModel::GetWordById(TWordId wid) const {
    if (wid >= IdToWord.size()) {
        return TWord();
    }
    return TWord(*IdToWord[wid]);
}

TCount TLangModel::GetWordCount(TWordId wid) const {
    return GetGram1HashCount(wid);
}

TWord TLangModel::GetWord(const std::wstring& word) const {
    auto it = WordToId.find(word);
    if (it != WordToId.end()) {
        return TWord(&it->first[0], it->first.size());
    }
    return TWord();
}

const std::unordered_set<wchar_t>& TLangModel::GetAlphabet() const {
    return Tokenizer.GetAlphabet();
}

TSentences TLangModel::Tokenize(const std::wstring& text) const {
    return Tokenizer.Process(text);
}

double TLangModel::GetGram1Prob(TWordId word) const {
    double countsGram1 = GetGram1HashCount(word);
    countsGram1 += K;
    double vocabSize = Grams1.size();
    return countsGram1 / (TotalWords + vocabSize);
}

double TLangModel::GetGram2Prob(TWordId word1, TWordId word2) const {
    double countsGram1 = GetGram1HashCount(word1);
    double countsGram2 = GetGram2HashCount(word1, word2);
    if (countsGram2 > countsGram1) { // (hash collision)
        countsGram2 = 0;
    }
    countsGram1 += TotalWords;
    countsGram2 += K;
    return countsGram2 / countsGram1;
}

double TLangModel::GetGram3Prob(TWordId word1, TWordId word2, TWordId word3) const {
    double countsGram2 = GetGram2HashCount(word1, word2);
    double countsGram3 = GetGram3HashCount(word1, word2, word3);
    if (countsGram3 > countsGram2) { // hash collision
        countsGram3 = 0;
    }
    countsGram2 += TotalWords;
    countsGram3 += K;
    return countsGram3 / countsGram2;
}

template<typename T>
TCount GetGramHashCount(T key,
                        const TPerfectHash& ph,
                        const std::vector<std::pair<uint32_t, TCount>>& buckets)
{
    std::string s = DumpKey(key);
    uint32_t bucket = ph.Hash(s);
    assert(bucket < ph.BucketsNumber());
    const std::pair<uint32_t, TCount>& data = buckets[bucket];
    if (data.first == CityHash32(&s[0], s.size())) {
        return data.second;
    }
    return TCount();
}

TCount TLangModel::GetGram1HashCount(TWordId word) const {
    if (word == UnknownWordId) {
        return TCount();
    }
    TGram1Key key = word;
    return GetGramHashCount(key, PerfectHash, Buckets);
}

TCount TLangModel::GetGram2HashCount(TWordId word1, TWordId word2) const {
    if (word1 == UnknownWordId || word2 == UnknownWordId) {
        return TCount();
    }
    TGram2Key key({word1, word2});
    return GetGramHashCount(key, PerfectHash, Buckets);
}

TCount TLangModel::GetGram3HashCount(TWordId word1, TWordId word2, TWordId word3) const {
    if (word1 == UnknownWordId || word2 == UnknownWordId || word3 == UnknownWordId) {
        return TCount();
    }
    TGram3Key key({word1, word2, word3});
    return GetGramHashCount(key, PerfectHash, Buckets);
}

} // NOpenSpell
