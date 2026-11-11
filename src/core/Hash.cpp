#include "Hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <stdexcept>
#include <vector>

#include "Text.h"

namespace modelhub {
namespace {

class Sha256Hasher {
public:
    Sha256Hasher() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0)))
            throw std::runtime_error("BCryptCreateHash failed");
    }
    ~Sha256Hasher() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void Update(const void* data, size_t n) {
        if (!BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0)))
            throw std::runtime_error("BCryptHashData failed");
    }
    std::string HexDigest() {
        unsigned char digest[32];
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, sizeof digest, 0)))
            throw std::runtime_error("BCryptFinishHash failed");
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char c : digest) {
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

}  // namespace

std::string Sha256File(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + PathToUtf8(p));
    Sha256Hasher h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto got = f.gcount();
        if (got > 0) h.Update(buf.data(), static_cast<size_t>(got));
    }
    return h.HexDigest();
}

std::string Sha256(std::string_view data) {
    Sha256Hasher h;
    h.Update(data.data(), data.size());
    return h.HexDigest();
}

}  // namespace modelhub
