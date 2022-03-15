#pragma once

#include "Source/Allocator.hpp"
#include "Source/AllocatorData.hpp"
#include "Source/AllocatorUtils.hpp"
#include "Source/Assert.hpp"
#include "Source/Policies/BoundsCheckPolicy.hpp"
#include "Source/Policies/MultithreadedPolicy.hpp"
#include "Source/Policies/Policies.hpp"
#include "Source/Utility/Alignment.hpp"

#define NO_DISCARD_ALLOC_INFO "Not using the pointer returned will cause a soft memory leak!"

namespace Memarena
{

namespace Internal
{
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
class StackPtr : public Ptr<T>
{
    template <StackAllocatorPolicy policy>
    friend class StackAllocator;

  public:
  private:
    inline StackPtr(T* _ptr, const Internal::StackHeader& _header) : Ptr<T>(_ptr), header(_header) {}
    inline StackPtr(T* _ptr, Offset startOffset, Offset endOffset) : Ptr<T>(_ptr), header(startOffset, endOffset) {}

    Internal::StackHeader header;
};

template <typename T>
class StackArrayPtr : public Ptr<T>
{
    template <StackAllocatorPolicy policy>
    friend class StackAllocator;

  public:
    [[nodiscard]] inline Size GetCount() const { return header.count; }
    T                         operator[](int index) const { return this->GetPtr()[index]; }

  private:
    StackArrayPtr(T* _ptr, const Internal::StackArrayHeader& _header) : Ptr<T>(_ptr), header(_header) {}
    StackArrayPtr(T* _ptr, Offset startOffset, Offset endOffset) : Ptr<T>(_ptr), header(startOffset, endOffset) {}

    Internal::StackArrayHeader header;
};

/**
 * @brief A custom memory allocator which allocates in a stack-like manner.
 * All the memory will be allocated up-front. This means it will have
 * zero allocations during runtime. This also means that it will take the same
 * amount of memory whether it is full or empty. Allocations and Deallocations
 * also need to be done in a stack-like manner. It is the responsibility of the
 * user to make sure that deallocations happen in an order that is the reverse
 * of the allocation order. If a pointer p1 that was not allocated last is deallocated,
 * future allocations will overwrite the memory of all allocations that were made
 * between the allocation and deallocation of p1.
 *
 * Space complexity is O(N*H) --> O(N) where H is the Header size and N is the number of allocations
 * Allocation and deallocation complexity: O(1)
 *
 * @tparam policy The StackAllocatorPolicy object to define the behaviour of this allocator
 */
template <StackAllocatorPolicy policy = StackAllocatorPolicy::Default>
class StackAllocator : public Internal::Allocator
{
  private:
    using HeaderBase = typename std::conditional<PolicyContains(policy, StackAllocatorPolicy::StackCheck), Internal::SafeHeaderBase,
                                                 Internal::UnsafeHeaderBase>::type;

    struct InplaceHeader : public HeaderBase
    {
        Offset startOffset;

        InplaceHeader(Offset _startOffset, Offset _endOffset) : HeaderBase(_endOffset), startOffset(_startOffset) {}
    };

    using Header             = Internal::StackHeader;
    using InplaceArrayHeader = Internal::StackArrayHeader;
    using ArrayHeader        = Internal::StackArrayHeader;

    using ThreadPolicy = MultithreadedPolicy<policy>;

    template <typename SyncPrimitive>
    using LockGuard = typename ThreadPolicy::template LockGuard<SyncPrimitive>;
    using Mutex     = typename ThreadPolicy::Mutex;

  public:
    // Prohibit default construction, moving and assignment
    StackAllocator()                      = delete;
    StackAllocator(StackAllocator&)       = delete;
    StackAllocator(const StackAllocator&) = delete;
    StackAllocator(StackAllocator&&)      = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;
    StackAllocator& operator=(StackAllocator&&) = delete;

    explicit StackAllocator(const Size totalSize, const std::shared_ptr<MemoryManager> memoryManager = nullptr,
                            const std::string& debugName = "StackAllocator")
        : Internal::Allocator(totalSize, memoryManager, debugName), m_StartAddress(std::bit_cast<UIntPtr>(GetStartPtr())),
          m_EndAddress(m_StartAddress + totalSize)
    {
    }

    ~StackAllocator() = default;

    template <typename Object, typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] StackPtr<Object> New(Args&&... argList)
    {
        auto [voidPtr, startOffset, endOffset] = AllocateInternal<0>(sizeof(Object), AlignOf(alignof(Object)));
        Object* objectPtr                      = new (voidPtr) Object(std::forward<Args>(argList)...);
        return StackPtr<Object>(objectPtr, startOffset, endOffset);
    }

    template <typename Object, typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] Object* NewRaw(Args&&... argList)
    {
        void*   voidPtr   = Allocate<Object>();
        Object* objectPtr = new (voidPtr) Object(std::forward<Args>(argList)...);
        return objectPtr;
    }

    template <typename Object>
    void Delete(StackPtr<Object> ptr)
    {
        Deallocate(StackPtr<void>(ptr.GetPtr(), ptr.header));
        ptr->~Object();
    }

    template <typename Object>
    void Delete(Object* ptr)
    {
        Deallocate(ptr);
        ptr->~Object();
    }

    template <typename Object, typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] StackArrayPtr<Object> NewArray(const Size objectCount, Args&&... argList)
    {
        auto [voidPtr, startOffset, endOffset] = AllocateInternal<0>(objectCount * sizeof(Object), AlignOf(alignof(Object)));
        return StackArrayPtr<Object>(Internal::ConstructArray<Object>(voidPtr, objectCount, std::forward<Args>(argList)...), startOffset,
                                     objectCount);
    }

    template <typename Object, typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] Object* NewArrayRaw(const Size objectCount, Args&&... argList)
    {
        void* voidPtr = AllocateArray<Object>(objectCount);
        return Internal::ConstructArray<Object>(voidPtr, objectCount, std::forward<Args>(argList)...);
    }

    template <typename Object>
    void DeleteArray(Object* ptr)
    {
        const Size objectCount = DeallocateArray(ptr, sizeof(Object));
        Internal::DestructArray(ptr, objectCount);
    }

    template <typename Object>
    void DeleteArray(StackArrayPtr<Object> ptr)
    {
        const Size objectCount = DeallocateArray(StackArrayPtr<void>(ptr.GetPtr(), ptr.header), sizeof(Object));
        Internal::DestructArray(ptr.GetPtr(), objectCount);
    }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* Allocate(const Size size, const Alignment& alignment)
    {
        auto [voidPtr, startOffset, endOffset] = AllocateInternal<sizeof(InplaceHeader)>(size, alignment);
        Internal::AllocateHeader<InplaceHeader>(voidPtr, startOffset, endOffset);
        return voidPtr;
    }

    template <typename Object>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* Allocate()
    {
        return Allocate(sizeof(Object), AlignOf(alignof(Object)));
    }

    void Deallocate(void* ptr)
    {
        const UIntPtr currentAddress = GetAddressFromPtr(ptr);
        UIntPtr       addressMarker  = currentAddress;
        InplaceHeader header         = Internal::GetHeaderFromPtr<InplaceHeader>(addressMarker);
        DeallocateInternal(currentAddress, addressMarker, header);
    }

    void Deallocate(const StackPtr<void>& ptr)
    {
        const void*   voidPtr        = ptr.GetPtr();
        const UIntPtr currentAddress = GetAddressFromPtr(voidPtr);
        DeallocateInternal(currentAddress, currentAddress, ptr.header);
    }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* AllocateArray(const Size objectCount, const Size objectSize, const Alignment& alignment)
    {
        const Size allocationSize              = objectCount * objectSize;
        auto [voidPtr, startOffset, endOffset] = AllocateInternal<sizeof(InplaceArrayHeader)>(allocationSize, alignment);
        Internal::AllocateHeader<InplaceArrayHeader>(voidPtr, startOffset, objectCount);
        return voidPtr;
    }

    template <typename Object>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* AllocateArray(const Size objectCount)
    {
        return AllocateArray(objectCount, sizeof(Object), AlignOf(alignof(Object)));
    }

    Size DeallocateArray(void* ptr, const Size objectSize)
    {
        const UIntPtr            currentAddress = GetAddressFromPtr(ptr);
        UIntPtr                  addressMarker  = currentAddress;
        const InplaceArrayHeader header         = Internal::GetHeaderFromPtr<InplaceArrayHeader>(addressMarker);
        DeallocateInternal(
            currentAddress, addressMarker,
            Header(header.startOffset, Internal::GetArrayEndOffset(currentAddress, m_StartAddress, header.count, objectSize)));
        return header.count;
    }

    Size DeallocateArray(const StackArrayPtr<void>& ptr, const Size objectSize)
    {
        const void*   voidPtr        = ptr.GetPtr();
        const UIntPtr currentAddress = GetAddressFromPtr(voidPtr);
        DeallocateInternal(
            currentAddress, currentAddress,
            Header(ptr.header.startOffset, Internal::GetArrayEndOffset(currentAddress, m_StartAddress, ptr.header.count, objectSize)));
        return ptr.header.count;
    }

    /**
     * @brief Resets the allocator to its initial state. Any further allocations
     * will possibly overwrite all object allocated prior to calling this method.
     * So make sure to only call this when you don't need any objects previously
     * allocated by this allocator.
     *
     */
    inline void Reset() { SetCurrentOffset(0); };

  private:
    template <Size headerSize>
    std::tuple<void*, Offset, Offset> AllocateInternal(const Size size, const Alignment& alignment)
    {
        LockGuard<Mutex> guard(m_MultithreadedPolicy.m_Mutex);

        const Offset  startOffset = m_CurrentOffset;
        const UIntPtr baseAddress = m_StartAddress + m_CurrentOffset;

        Padding padding{0};
        UIntPtr alignedAddress{0};

        constexpr Size totalHeaderSize = GetTotalHeaderSize<headerSize>();

        if constexpr (totalHeaderSize > 0)
        {
            padding        = CalculateAlignedPaddingWithHeader(baseAddress, alignment, totalHeaderSize);
            alignedAddress = baseAddress + padding;
        }
        else
        {
            alignedAddress = CalculateAlignedAddress(baseAddress, alignment);
            padding        = alignedAddress - baseAddress;
        }

        Size totalSizeAfterAllocation = m_CurrentOffset + padding + size;

        if constexpr (PolicyContains(policy, StackAllocatorPolicy::SizeCheck))
        {
            MEMARENA_ASSERT(totalSizeAfterAllocation <= GetTotalSize(), "Error: The allocator %s is out of memory!\n",
                            GetDebugName().c_str());
        }

        if constexpr (PolicyContains(policy, StackAllocatorPolicy::BoundsCheck))
        {
            totalSizeAfterAllocation += sizeof(BoundGuardBack);

            const UIntPtr frontGuardAddress = alignedAddress - totalHeaderSize;
            const UIntPtr backGuardAddress  = alignedAddress + size;

            new (std::bit_cast<void*>(frontGuardAddress)) BoundGuardFront(m_CurrentOffset, size);
            new (std::bit_cast<void*>(backGuardAddress)) BoundGuardBack(m_CurrentOffset);
        }

        SetCurrentOffset(totalSizeAfterAllocation);

        const Offset endOffset = m_CurrentOffset;

        void* allocatedPtr = std::bit_cast<void*>(alignedAddress);

        return {allocatedPtr, startOffset, endOffset};
    }

    template <typename Header>
    void DeallocateInternal(const UIntPtr address, const UIntPtr addressMarker, const Header& header)
    {
        LockGuard<Mutex> guard(m_MultithreadedPolicy.m_Mutex);

        const Offset newOffset = header.startOffset;

        if constexpr (PolicyContains(policy, StackAllocatorPolicy::StackCheck))
        {
            MEMARENA_ASSERT(header.endOffset == m_CurrentOffset, "Error: Attempt to deallocate in wrong order in the stack allocator %s!\n",
                            GetDebugName().c_str());
        }

        if constexpr (PolicyContains(policy, StackAllocatorPolicy::BoundsCheck))
        {
            const UIntPtr          frontGuardAddress = addressMarker - sizeof(BoundGuardFront);
            const BoundGuardFront* frontGuard        = std::bit_cast<BoundGuardFront*>(frontGuardAddress);

            const UIntPtr         backGuardAddress = address + frontGuard->allocationSize;
            const BoundGuardBack* backGuard        = std::bit_cast<BoundGuardBack*>(backGuardAddress);

            MEMARENA_ASSERT(frontGuard->offset == newOffset && backGuard->offset == newOffset,
                            "Error: Memory stomping detected in allocator %s at offset %d and address %d!\n", GetDebugName().c_str(),
                            newOffset, address);
        }

        SetCurrentOffset(newOffset);
    }

    UIntPtr GetAddressFromPtr(const void* ptr) const
    {
        if constexpr (PolicyContains(policy, StackAllocatorPolicy::NullCheck))
        {
            MEMARENA_ASSERT(ptr, "Error: Cannot deallocate nullptr!\n");
        }

        const UIntPtr address = std::bit_cast<UIntPtr>(ptr);

        if constexpr (PolicyContains(policy, StackAllocatorPolicy::OwnershipCheck))
        {
            MEMARENA_ASSERT(OwnsAddress(address), "Error: The allocator %s does not own the pointer %d!\n", GetDebugName().c_str(),
                            address);
        }

        return address;
    }

    template <Size headerSize>
    static consteval Size GetTotalHeaderSize()
    {
        if constexpr (PolicyContains(policy, StackAllocatorPolicy::BoundsCheck))
        {
            return headerSize + sizeof(BoundGuardFront);
        }
        else
        {
            return headerSize;
        }
    }

    void SetCurrentOffset(const Offset offset)
    {
        m_CurrentOffset = offset;
        SetUsedSize(offset);
    }

    [[nodiscard]] bool OwnsAddress(UIntPtr address) const { return address >= m_StartAddress && address <= m_EndAddress; }

    ThreadPolicy m_MultithreadedPolicy;

    UIntPtr m_StartAddress;
    UIntPtr m_EndAddress;
    Offset  m_CurrentOffset = 0;
};

template <typename Object, StackAllocatorPolicy policy = StackAllocatorPolicy::Default>
class StackAllocatorTemplated
{
  public:
    StackAllocatorTemplated()                               = delete;
    StackAllocatorTemplated(StackAllocatorTemplated&)       = delete;
    StackAllocatorTemplated(const StackAllocatorTemplated&) = delete;
    StackAllocatorTemplated(StackAllocatorTemplated&&)      = delete;
    StackAllocatorTemplated& operator=(const StackAllocatorTemplated&) = delete;
    StackAllocatorTemplated& operator=(StackAllocatorTemplated&&) = delete;

    explicit StackAllocatorTemplated(const Size totalSize, const std::shared_ptr<MemoryManager> memoryManager = nullptr,
                                     const std::string& debugName = "StackAllocator")
        : m_StackAllocator(totalSize, memoryManager, debugName)
    {
    }

    ~StackAllocatorTemplated() = default;

    template <typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] StackPtr<Object> New(Args&&... argList)
    {
        return m_StackAllocator.template New<Object>(std::forward<Args>(argList)...);
    }

    template <typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] Object* NewRaw(Args&&... argList)
    {
        return m_StackAllocator.template NewRaw<Object>(std::forward<Args>(argList)...);
    }

    void Delete(StackPtr<Object> ptr) { m_StackAllocator.Delete(ptr); }

    void Delete(Object* ptr) { m_StackAllocator.Delete(ptr); }

    template <typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] StackArrayPtr<Object> NewArray(const Size objectCount, Args&&... argList)
    {
        return m_StackAllocator.template NewArray<Object>(objectCount, std::forward<Args>(argList)...);
    }

    template <typename... Args>
    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] Object* NewArrayRaw(const Size objectCount, Args&&... argList)
    {
        return m_StackAllocator.template NewArrayRaw<Object>(objectCount, std::forward<Args>(argList)...);
    }

    void DeleteArray(Object* ptr) { m_StackAllocator.DeleteArray(ptr); }

    void DeleteArray(StackArrayPtr<Object> ptr) { m_StackAllocator.DeleteArray(ptr); }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* Allocate(const Size size, const Alignment& alignment)
    {
        return m_StackAllocator.Allocate(size, alignment);
    }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* Allocate() { return m_StackAllocator.Allocate(sizeof(Object), AlignOf(alignof(Object))); }

    void Deallocate(void* ptr) { m_StackAllocator.Deallocate(ptr); }

    void Deallocate(const StackPtr<void>& ptr) { m_StackAllocator.Deallocate(ptr); }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* AllocateArray(const Size objectCount, const Size objectSize, const Alignment& alignment)
    {
        return m_StackAllocator.AllocateArray(objectCount, objectSize, alignment);
    }

    [[nodiscard(NO_DISCARD_ALLOC_INFO)]] void* AllocateArray(const Size objectCount)
    {
        return AllocateArray(objectCount, sizeof(Object), AlignOf(alignof(Object)));
    }

    Size DeallocateArray(void* ptr, const Size objectSize) { return m_StackAllocator.DeallocateArray(ptr, objectSize); }

    Size DeallocateArray(const StackArrayPtr<void>& ptr, const Size objectSize)
    {
        return m_StackAllocator.DeallocateArray(ptr, objectSize);
    }

    /**
     * @brief Resets the allocator to its initial state. Any further allocations
     * will possibly overwrite all object allocated prior to calling this method.
     * So make sure to only call this when you don't need any objects previously
     * allocated by this allocator.
     *
     */
    inline void Reset() { m_StackAllocator.Reset(); }

    [[nodiscard]] Size        GetUsedSize() const {return m_StackAllocator.GetUsedSize();}
    [[nodiscard]] Size        GetTotalSize() const{return m_StackAllocator.GetTotalSize();}
    [[nodiscard]] std::string GetDebugName() const{return m_StackAllocator.GetDebugName();}

  private:
    StackAllocator<policy> m_StackAllocator;
};

} // namespace Memarena