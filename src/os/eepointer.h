#pragma once

#include <stdint.h>

#ifdef RESONANCE_HOST
/**
 * A pointer member of a record the game reads whole from the disc, kept to the Emotion Engine's
 * 32 bits so that the record has the disc's layout.
 *
 * The native build places every block the game addresses below 4 GiB, so the low 32 bits of a
 * host pointer are the whole pointer.
 */
template <class T>
class EePointer {
public:
    EePointer() = default;

    EePointer(T *pTarget) : mAddress(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pTarget))) {
    }

    operator T *() const {
        return reinterpret_cast<T *>(static_cast<uintptr_t>(mAddress));
    }

    template <class U>
    explicit operator U *() const {
        return static_cast<U *>(static_cast<T *>(*this));
    }

    T *operator->() const {
        return *this;
    }

private:
    uint32_t mAddress;
};

/** An address as a 32-bit integer, for a record of the Emotion Engine's layout. */
using EeAddress = uint32_t;
#else
template <class T>
using EePointer = T *;

using EeAddress = uintptr_t;
#endif
