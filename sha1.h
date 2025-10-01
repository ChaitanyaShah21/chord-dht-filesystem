#ifndef SIMPLE_SHA1_H
#define SIMPLE_SHA1_H

#include <string>
#include<openssl/sha.h>
#include <sstream>
#include<iomanip>

//convert raw bytes to hex string
inline std::string to_hex(const unsigned char* hash, size_t len)
{
    std::ostringstream oss;
    oss<<std::hex <<std::setfill('0');

    for(size_t i=0;i<len;++i)
    {
        oss<<std::setw(2)<< static_cast<int>(hash[i]);
    }

    return oss.str();
}

//hash from string
inline std::string sha1(const std::string &input)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()),input.size(),hash);
    return to_hex(hash, SHA_DIGEST_LENGTH);
}

inline std::string sha1_bytes(const unsigned char* data, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(data,len,hash);
    return to_hex(hash, SHA_DIGEST_LENGTH);
}

#endif