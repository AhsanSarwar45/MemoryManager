#pragma once

#include <memory>
#include <string>

#include "Source/TypeAliases.hpp"
#include "StackAllocatorUtils.hpp"

namespace Memarena
{

class MemoryManager;
struct AllocatorData;

namespace Internal
{
class StackAllocatorBase
{
  public:
    /**
     * @brief Resets the allocator to its initial state. Any further allocations
     * will possibly overwrite all object allocated prior to calling this method.
     * So make sure to only call this when you don't need any objects previously
     * allocated by this allocator.
     *
     */
    void Reset();

    Size        GetUsedSize() const;
    Size        GetTotalSize() const;
    std::string GetDebugName() const;

  protected:
    StackAllocatorBase(const Size totalSize, const std::shared_ptr<MemoryManager> memoryManager, const std::string& debugName);

    ~StackAllocatorBase();

    void SetCurrentOffset(Offset offset);

    bool OwnsAddress(UIntPtr address);

  protected:
    std::shared_ptr<MemoryManager> m_MemoryManager; // Pointer to the memory manager that this allocator will report the memory usage to
    std::shared_ptr<AllocatorData> m_Data;
    void*                          m_StartPtr = nullptr;
    UIntPtr                        m_StartAddress;
    UIntPtr                        m_EndAddress;
    Offset                         m_CurrentOffset;

  protected:
};

struct SafeHeaderBase
{
    Offset endOffset;

    explicit SafeHeaderBase(Offset _endOffset) : endOffset(_endOffset) {}
};
struct UnsafeHeaderBase
{
    explicit UnsafeHeaderBase(Offset _endOffset) {}
};

struct StackHeader
{
    Offset startOffset;
    Offset endOffset;

    StackHeader(Offset _startOffset, Offset _endOffset) : startOffset(_startOffset), endOffset(_endOffset) {}
};

struct StackArrayHeader
{
    Offset startOffset;
    Offset count;

    StackArrayHeader(Offset _startOffset, Offset _count) : startOffset(_startOffset), count(_count) {}
};

} // namespace Internal

template <typename T>
struct Ptr
{
  public:
    inline T* GetPtr() const { return ptr; }

    inline T*       operator->() const { return ptr; }
    inline explicit operator bool() const noexcept { return (ptr != nullptr); }

  protected:
    T* ptr;

    explicit Ptr(T* _ptr) : ptr(_ptr) {}
};

template <typename T>
class StackPtr : public Ptr<T>
{
    template <StackAllocatorPolicy allocatorPolicy>
    friend class StackAllocator;

  private:
    inline StackPtr(T* _ptr, Internal::StackHeader _header) : Ptr<T>(_ptr), header(_header) {}

  private:
    Internal::StackHeader header;
};

template <typename T>
class StackArrayPtr : public Ptr<T>
{
    template <StackAllocatorPolicy allocatorPolicy>
    friend class StackAllocator;

  public:
    Size GetCount() const { return header.count; }
    T    operator[](int index) const { return this->ptr[index]; }

  private:
    StackArrayPtr(T* _ptr, Internal::StackArrayHeader _header) : Ptr<T>(_ptr), header(_header) {}

  private:
    Internal::StackArrayHeader header;
};

} // namespace Memarena