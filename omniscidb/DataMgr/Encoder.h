#ifndef ENCODER_H
#define ENCODER_H

#include <vector>
#include <iostream>
#include <stdexcept>
#include <limits>
#include "../../Shared/types.h"
#include "../../Shared/sqltypes.h"

namespace Memory_Namespace {
    class AbstractBuffer;
}


class Encoder {
    public: 
        static Encoder * Create(Memory_Namespace::AbstractBuffer * buffer, const SQLTypes sqlType, const EncodingType encodingType, const EncodedDataType encodedDataType);
        Encoder(Memory_Namespace::AbstractBuffer * buffer): buffer_(buffer) {}
        virtual void appendData(mapd_addr_t srcData, const mapd_size_t numElems) = 0;

    protected:
        Memory_Namespace::AbstractBuffer * buffer_;
};




#endif // Encoder_h
