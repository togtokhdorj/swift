//===--- ProtocolConformance.cpp - Swift protocol conformance checking ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Checking and caching of Swift protocol conformances.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "swift/Basic/Lazy.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Mutex.h"
#include "swift/Runtime/Unreachable.h"
#include "ImageInspection.h"
#include "Private.h"

#include <vector>

using namespace swift;

#ifndef NDEBUG
template <> void ProtocolDescriptor::dump() const {
  unsigned NumInheritedProtocols =
      InheritedProtocols ? InheritedProtocols->NumProtocols : 0;

  printf("TargetProtocolDescriptor.\n"
         "Name: \"%s\".\n"
         "ObjC Isa: %p.\n",
         Name, _ObjC_Isa);
  Flags.dump();
  printf("Has Inherited Protocols: %s.\n",
         (NumInheritedProtocols ? "true" : "false"));
  if (NumInheritedProtocols) {
    printf("Inherited Protocol List:\n");
    for (unsigned i = 0, e = NumInheritedProtocols; i != e; ++i) {
      printf("%s\n", (*InheritedProtocols)[i]->Name);
    }
  }
}

void ProtocolDescriptorFlags::dump() const {
  printf("ProtocolDescriptorFlags.\n");
  printf("Is Swift: %s.\n", (isSwift() ? "true" : "false"));
  printf("Needs Witness Table: %s.\n",
         (needsWitnessTable() ? "true" : "false"));
  printf("Is Resilient: %s.\n", (isResilient() ? "true" : "false"));
  printf("Special Protocol: %s.\n",
         (bool(getSpecialProtocol()) ? "Error" : "None"));
  printf("Class Constraint: %s.\n",
         (bool(getClassConstraint()) ? "Class" : "Any"));
  printf("Dispatch Strategy: %s.\n",
         (bool(getDispatchStrategy()) ? "Swift" : "ObjC"));
}

#endif

#if !defined(NDEBUG) && SWIFT_OBJC_INTEROP
#include <objc/runtime.h>

static const char *class_getName(const ClassMetadata* type) {
  return class_getName(
    reinterpret_cast<Class>(const_cast<ClassMetadata*>(type)));
}

template<> void ProtocolConformanceDescriptor::dump() const {
  auto symbolName = [&](const void *addr) -> const char * {
    SymbolInfo info;
    int ok = lookupSymbol(addr, &info);
    if (!ok)
      return "<unknown addr>";
    return info.symbolName;
  };

  switch (auto kind = getTypeKind()) {
    case TypeMetadataRecordKind::Reserved:
      printf("unknown (reserved)");
      break;

    case TypeMetadataRecordKind::IndirectObjCClass:
      printf("indirect Objective-C class %s",
             class_getName(*getIndirectObjCClass()));
      break;
      
    case TypeMetadataRecordKind::DirectNominalTypeDescriptor:
    case TypeMetadataRecordKind::IndirectNominalTypeDescriptor:
      printf("unique nominal type descriptor %s", symbolName(getTypeContextDescriptor()));
      break;
  }
  
  printf(" => ");
  
  switch (getConformanceKind()) {
    case ConformanceFlags::ConformanceKind::WitnessTable:
      printf("witness table %s\n", symbolName(getStaticWitnessTable()));
      break;
    case ConformanceFlags::ConformanceKind::WitnessTableAccessor:
      printf("witness table accessor %s\n",
             symbolName((const void *)(uintptr_t)getWitnessTableAccessor()));
      break;
    case ConformanceFlags::ConformanceKind::ConditionalWitnessTableAccessor:
      printf("conditional witness table accessor %s\n",
             symbolName((const void *)(uintptr_t)getWitnessTableAccessor()));
      break;
  }
}
#endif

#ifndef NDEBUG
template<> void ProtocolConformanceDescriptor::verify() const {
  auto typeKind = unsigned(getTypeKind());
  assert(((unsigned(TypeMetadataRecordKind::First_Kind) <= typeKind) &&
          (unsigned(TypeMetadataRecordKind::Last_Kind) >= typeKind)) &&
         "Corrupted type metadata record kind");

  auto confKind = unsigned(getConformanceKind());
  using ConformanceKind = ConformanceFlags::ConformanceKind;
  assert(((unsigned(ConformanceKind::First_Kind) <= confKind) &&
          (unsigned(ConformanceKind::Last_Kind) >= confKind)) &&
         "Corrupted conformance kind");
}
#endif

/// Take the type reference inside a protocol conformance record and fetch the
/// canonical metadata pointer for the type it refers to.
/// Returns nil for universal or generic type references.
template <>
const Metadata *
ProtocolConformanceDescriptor::getCanonicalTypeMetadata() const {
  switch (getTypeKind()) {
  case TypeMetadataRecordKind::Reserved:
    return nullptr;
  case TypeMetadataRecordKind::IndirectObjCClass:
    // The class may be ObjC, in which case we need to instantiate its Swift
    // metadata. The class additionally may be weak-linked, so we have to check
    // for null.
    if (auto *ClassMetadata = *getIndirectObjCClass())
      return getMetadataForClass(ClassMetadata);
    return nullptr;
      
  case TypeMetadataRecordKind::DirectNominalTypeDescriptor:
  case TypeMetadataRecordKind::IndirectNominalTypeDescriptor:
    return nullptr;
  }

  swift_runtime_unreachable("Unhandled TypeMetadataRecordKind in switch.");
}

template<>
const WitnessTable *
ProtocolConformanceDescriptor::getWitnessTable(const Metadata *type) const {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
  switch (getConformanceKind()) {
  case ConformanceFlags::ConformanceKind::WitnessTable:
    return getStaticWitnessTable();

  case ConformanceFlags::ConformanceKind::WitnessTableAccessor:
    return getWitnessTableAccessor()(type, nullptr, 0);

  case ConformanceFlags::ConformanceKind::ConditionalWitnessTableAccessor: {
    // FIXME: this needs to query the conditional requirements to form the
    // array of witness tables to pass along to the accessor.

    // Pretty-print the type name.
    auto typeNamePair = TwoWordPair<const char *, uintptr_t>(
      swift_getTypeName(type, /*qualified=*/true));
    std::string typeName(typeNamePair.first,
                         typeNamePair.first + typeNamePair.second);

    // Demangle the protocol name.
    DemangleOptions options;
    options.DisplayEntityTypes = false;
    std::string demangledProtocolName =
      demangleSymbolAsString(StringRef(getProtocol()->Name), options);

    warning(/*flag=*/0,
            "warning: Swift runtime does not yet support dynamically "
            "querying conditional conformance ('%s': '%s')\n",
            typeName.c_str(), demangledProtocolName.c_str());
    return nullptr;
  }

  default:
    return nullptr;
  }
#pragma clang diagnostic pop

}

namespace {
  struct ConformanceSection {
    const ProtocolConformanceRecord *Begin, *End;
    const ProtocolConformanceRecord *begin() const {
      return Begin;
    }
    const ProtocolConformanceRecord *end() const {
      return End;
    }
  };

  struct ConformanceCacheKey {
    /// Either a Metadata* or a NominalTypeDescriptor*.
    const void *Type;
    const ProtocolDescriptor *Proto;

    ConformanceCacheKey(const void *type, const ProtocolDescriptor *proto)
      : Type(type), Proto(proto) {}
  };

  struct ConformanceCacheEntry {
  private:
    const void *Type; 
    const ProtocolDescriptor *Proto;
    std::atomic<const WitnessTable *> Table;
    std::atomic<uintptr_t> FailureGeneration;

  public:
    ConformanceCacheEntry(ConformanceCacheKey key,
                          const WitnessTable *table,
                          uintptr_t failureGeneration)
      : Type(key.Type), Proto(key.Proto), Table(table),
        FailureGeneration(failureGeneration) {
    }

    int compareWithKey(const ConformanceCacheKey &key) const {
      if (key.Type != Type) {
        return (uintptr_t(key.Type) < uintptr_t(Type) ? -1 : 1);
      } else if (key.Proto != Proto) {
        return (uintptr_t(key.Proto) < uintptr_t(Proto) ? -1 : 1);
      } else {
        return 0;
      }
    }

    template <class... Args>
    static size_t getExtraAllocationSize(Args &&... ignored) {
      return 0;
    }

    bool isSuccessful() const {
      return Table.load(std::memory_order_relaxed) != nullptr;
    }

    void makeSuccessful(const WitnessTable *table) {
      Table.store(table, std::memory_order_release);
    }

    void updateFailureGeneration(uintptr_t failureGeneration) {
      assert(!isSuccessful());
      FailureGeneration.store(failureGeneration, std::memory_order_relaxed);
    }
    
    /// Get the cached witness table, if successful.
    const WitnessTable *getWitnessTable() const {
      assert(isSuccessful());
      return Table.load(std::memory_order_acquire);
    }
    
    /// Get the generation number under which this lookup failed.
    unsigned getFailureGeneration() const {
      assert(!isSuccessful());
      return FailureGeneration.load(std::memory_order_relaxed);
    }
  };
} // end anonymous namespace

// Conformance Cache.
struct ConformanceState {
  ConcurrentMap<ConformanceCacheEntry> Cache;
  std::vector<ConformanceSection> SectionsToScan;
  Mutex SectionsToScanLock;
  
  ConformanceState() {
    SectionsToScan.reserve(16);
    initializeProtocolConformanceLookup();
  }

  void cacheSuccess(const void *type, const ProtocolDescriptor *proto,
                    const WitnessTable *witness) {
    auto result = Cache.getOrInsert(ConformanceCacheKey(type, proto),
                                    witness, uintptr_t(0));

    // If the entry was already present, we may need to update it.
    if (!result.second) {
      result.first->makeSuccessful(witness);
    }
  }

  void cacheFailure(const void *type, const ProtocolDescriptor *proto) {
    uintptr_t failureGeneration = SectionsToScan.size();
    auto result = Cache.getOrInsert(ConformanceCacheKey(type, proto),
                                    (const WitnessTable *) nullptr,
                                    failureGeneration);

    // If the entry was already present, we may need to update it.
    if (!result.second) {
      result.first->updateFailureGeneration(failureGeneration);
    }
  }

  ConformanceCacheEntry *findCached(const void *type,
                                    const ProtocolDescriptor *proto) {
    return Cache.find(ConformanceCacheKey(type, proto));
  }

#ifndef NDEBUG
  void verify() const LLVM_ATTRIBUTE_USED;
#endif
};

#ifndef NDEBUG
void ConformanceState::verify() const {
  // Iterate over all of the sections and verify all of the protocol
  // descriptors.
  auto &Self = const_cast<ConformanceState &>(*this);
  ScopedLock guard(Self.SectionsToScanLock);

  for (const auto &Section : SectionsToScan) {
    for (const auto &Record : Section) {
      Record.get()->verify();
    }
  }
}
#endif

static Lazy<ConformanceState> Conformances;

static void
_registerProtocolConformances(ConformanceState &C,
                              const ProtocolConformanceRecord *begin,
                              const ProtocolConformanceRecord *end) {
  ScopedLock guard(C.SectionsToScanLock);
  C.SectionsToScan.push_back(ConformanceSection{begin, end});
}

void swift::addImageProtocolConformanceBlockCallback(const void *conformances,
                                                   uintptr_t conformancesSize) {
  assert(conformancesSize % sizeof(ProtocolConformanceRecord) == 0 &&
         "conformances section not a multiple of ProtocolConformanceRecord");

  // If we have a section, enqueue the conformances for lookup.
  auto conformanceBytes = reinterpret_cast<const char *>(conformances);
  auto recordsBegin
    = reinterpret_cast<const ProtocolConformanceRecord*>(conformances);
  auto recordsEnd
    = reinterpret_cast<const ProtocolConformanceRecord*>
                                          (conformanceBytes + conformancesSize);
  
  // Conformance cache should always be sufficiently initialized by this point.
  _registerProtocolConformances(Conformances.unsafeGetAlreadyInitialized(),
                                recordsBegin, recordsEnd);
}

void
swift::swift_registerProtocolConformances(const ProtocolConformanceRecord *begin,
                                          const ProtocolConformanceRecord *end){
  auto &C = Conformances.get();
  _registerProtocolConformances(C, begin, end);
}


struct ConformanceCacheResult {
  // true if witnessTable is an authoritative result as-is.
  // false if more searching is required (for example, because a cached
  // failure was returned in failureEntry but it is out-of-date.
  bool isAuthoritative;

  // The matching witness table, or null if no cached conformance was found.
  const WitnessTable *witnessTable;

  // If the search fails, this may be the negative cache entry for the
  // queried type itself. This entry may be null or out-of-date.
  ConformanceCacheEntry *failureEntry;

  static ConformanceCacheResult
  cachedSuccess(const WitnessTable *table) {
    return ConformanceCacheResult { true, table, nullptr };
  }

  static ConformanceCacheResult
  cachedFailure(ConformanceCacheEntry *entry, bool auth) {
    return ConformanceCacheResult { auth, nullptr, entry };
  }

  static ConformanceCacheResult
  cacheMiss() {
    return ConformanceCacheResult { false, nullptr, nullptr };
  }
};

/// Search for a witness table in the ConformanceCache.
static
ConformanceCacheResult
searchInConformanceCache(const Metadata *type,
                         const ProtocolDescriptor *protocol) {
  auto &C = Conformances.get();
  auto origType = type;
  ConformanceCacheEntry *failureEntry = nullptr;

recur:
  {
    // Try the specific type first.
    if (auto *Value = C.findCached(type, protocol)) {
      if (Value->isSuccessful()) {
        // Found a conformance on the type or some superclass. Return it.
        return ConformanceCacheResult::cachedSuccess(Value->getWitnessTable());
      }

      // Found a negative cache entry.

      bool isAuthoritative;
      if (type == origType) {
        // This negative cache entry is for the original query type.
        // Remember it so it can be returned later.
        failureEntry = Value;
        // An up-to-date entry for the original type is authoritative.
        isAuthoritative = true;
      } else {
        // An up-to-date cached failure for a superclass of the type is not
        // authoritative: there may be a still-undiscovered conformance
        // for the original query type.
        isAuthoritative = false;
      }

      // Check if the negative cache entry is up-to-date.
      // FIXME: Using SectionsToScan.size() outside SectionsToScanLock
      // is undefined.
      if (Value->getFailureGeneration() == C.SectionsToScan.size()) {
        // Negative cache entry is up-to-date. Return failure along with
        // the original query type's own cache entry, if we found one.
        // (That entry may be out of date but the caller still has use for it.)
        return ConformanceCacheResult::cachedFailure(failureEntry,
                                                     isAuthoritative);
      }

      // Negative cache entry is out-of-date.
      // Continue searching for a better result.
    }
  }

  {
    // For generic and resilient types, nondependent conformances
    // are keyed by the nominal type descriptor rather than the
    // metadata, so try that.
    const auto *description = type->getTypeContextDescriptor();

    // Hash and lookup the type-protocol pair in the cache.
    if (auto *Value = C.findCached(description, protocol)) {
      if (Value->isSuccessful())
        return ConformanceCacheResult::cachedSuccess(Value->getWitnessTable());

      // We don't try to cache negative responses for generic
      // patterns.
    }
  }

  // If the type is a class, try its superclass.
  if (const ClassMetadata *classType = type->getClassObject()) {
    if (classHasSuperclass(classType)) {
      type = getMetadataForClass(classType->SuperClass);
      goto recur;
    }
  }

  // We did not find an up-to-date cache entry.
  // If we found an out-of-date entry for the original query type then
  // return it (non-authoritatively). Otherwise return a cache miss.
  if (failureEntry)
    return ConformanceCacheResult::cachedFailure(failureEntry, false);
  else
    return ConformanceCacheResult::cacheMiss();
}

/// Checks if a given candidate is a type itself, one of its
/// superclasses or a related generic type.
///
/// This check is supposed to use the same logic that is used
/// by searchInConformanceCache.
///
/// \param candidate Pointer to a Metadata or a NominalTypeDescriptor.
///
static
bool isRelatedType(const Metadata *type, const void *candidate,
                   bool candidateIsMetadata) {

  while (true) {
    // Check whether the types match.
    if (candidateIsMetadata && type == candidate)
      return true;

    // Check whether the nominal type descriptors match.
    if (!candidateIsMetadata) {
      const auto *description = type->getTypeContextDescriptor();
      auto candidateDescription =
      static_cast<const TypeContextDescriptor *>(candidate);
      if (description && equalContexts(description, candidateDescription))
        return true;
    }

    // If the type is a class, try its superclass.
    if (const ClassMetadata *classType = type->getClassObject()) {
      if (classHasSuperclass(classType)) {
        type = getMetadataForClass(classType->SuperClass);
        continue;
      }
    }

    break;
  }

  return false;
}

const WitnessTable *
swift::swift_conformsToProtocol(const Metadata * const type,
                                const ProtocolDescriptor *protocol) {
  auto &C = Conformances.get();

  // See if we have a cached conformance. The ConcurrentMap data structure
  // allows us to insert and search the map concurrently without locking.
  // We do lock the slow path because the SectionsToScan data structure is not
  // concurrent.
  auto FoundConformance = searchInConformanceCache(type, protocol);
  // If the result (positive or negative) is authoritative, return it.
  if (FoundConformance.isAuthoritative)
    return FoundConformance.witnessTable;

  auto failureEntry = FoundConformance.failureEntry;

  // No up-to-date cache entry found.
  // Acquire the lock so we can scan conformance records.
  ScopedLock guard(C.SectionsToScanLock);

  // The world may have changed while we waited for the lock.
  // If we found an out-of-date negative cache entry before
  // acquiring the lock, make sure the entry is still negative and out of date.
  // If we found no entry before acquiring the lock, search the cache again.
  if (failureEntry) {
    if (failureEntry->isSuccessful()) {
      // Somebody else found a conformance.
      return failureEntry->getWitnessTable();
    }
    if (failureEntry->getFailureGeneration() == C.SectionsToScan.size()) {
      // Somebody else brought the negative cache entry up to date.
      return nullptr;
    }
  }
  else {
    FoundConformance = searchInConformanceCache(type, protocol);
    if (FoundConformance.isAuthoritative) {
      // Somebody else found a conformance or cached an up-to-date failure.
      return FoundConformance.witnessTable;
    }
    failureEntry = FoundConformance.failureEntry;
  }

  // We are now caught up after acquiring the lock.
  // Prepare to scan conformance records.

  // Scan only sections that were not scanned yet.
  // If we found an out-of-date negative cache entry,
  // we need not to re-scan the sections that it covers.
  unsigned startSectionIdx =
    failureEntry ? failureEntry->getFailureGeneration() : 0;

  unsigned endSectionIdx = C.SectionsToScan.size();

  // If there are no unscanned sections outstanding
  // then we can cache failure and give up now.
  if (startSectionIdx == endSectionIdx) {
    C.cacheFailure(type, protocol);
    return nullptr;
  }

  // Really scan conformance records.

  for (unsigned sectionIdx = startSectionIdx;
       sectionIdx < endSectionIdx;
       ++sectionIdx) {
    auto &section = C.SectionsToScan[sectionIdx];
    // Eagerly pull records for nondependent witnesses into our cache.
    for (const auto &record : section) {
      auto &descriptor = *record.get();

      // If the record applies to a specific type, cache it.
      if (auto metadata = descriptor.getCanonicalTypeMetadata()) {
        auto P = descriptor.getProtocol();

        // Look for an exact match.
        if (protocol != P)
          continue;

        if (!isRelatedType(type, metadata, /*candidateIsMetadata=*/true))
          continue;

        // Store the type-protocol pair in the cache.
        auto witness = descriptor.getWitnessTable(metadata);
        if (witness) {
          C.cacheSuccess(metadata, P, witness);
        } else {
          C.cacheFailure(metadata, P);
        }

      // TODO: "Nondependent witness table" probably deserves its own flag.
      // An accessor function might still be necessary even if the witness table
      // can be shared.
      } else if (descriptor.getTypeKind()
                   == TypeMetadataRecordKind::DirectNominalTypeDescriptor ||
                 descriptor.getTypeKind()
                  == TypeMetadataRecordKind::IndirectNominalTypeDescriptor) {
        auto R = descriptor.getTypeContextDescriptor();
        auto P = descriptor.getProtocol();

        // Look for an exact match.
        if (protocol != P)
          continue;

        if (!isRelatedType(type, R, /*candidateIsMetadata=*/false))
          continue;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
        // Store the type-protocol pair in the cache.
        switch (descriptor.getConformanceKind()) {
        case ConformanceFlags::ConformanceKind::WitnessTable:
          // If the record provides a nondependent witness table for all
          // instances of a generic type, cache it for the generic pattern.
          C.cacheSuccess(type->getTypeContextDescriptor(), P,
                         descriptor.getStaticWitnessTable());
          break;

        case ConformanceFlags::ConformanceKind::WitnessTableAccessor:
        case ConformanceFlags::ConformanceKind::ConditionalWitnessTableAccessor:
          // If the record provides a dependent witness table accessor,
          // cache the result for the instantiated type metadata.
          C.cacheSuccess(type, P, descriptor.getWitnessTable(type));
          break;

        default:
          // Always fail, because we cannot interpret a future conformance
          // kind.
          C.cacheFailure(metadata, P);
          break;
        }
#pragma clang diagnostic pop
      }
    }
  }

  // Conformance scan is complete.
  // Search the cache once more, and this time update the cache if necessary.

  FoundConformance = searchInConformanceCache(type, protocol);
  if (FoundConformance.isAuthoritative) {
    return FoundConformance.witnessTable;
  } else {
    C.cacheFailure(type, protocol);
    return nullptr;
  }
}

const TypeContextDescriptor *
swift::_searchConformancesByMangledTypeName(Demangle::NodePointer node) {
  auto &C = Conformances.get();

  ScopedLock guard(C.SectionsToScanLock);

  unsigned sectionIdx = 0;
  unsigned endSectionIdx = C.SectionsToScan.size();

  for (; sectionIdx < endSectionIdx; ++sectionIdx) {
    auto &section = C.SectionsToScan[sectionIdx];
    for (const auto &record : section) {
      if (auto ntd = record->getTypeContextDescriptor()) {
        if (_contextDescriptorMatchesMangling(ntd, node))
          return ntd;
      }
    }
  }

  return nullptr;
}

/// Resolve a reference to a generic parameter to type metadata.
static const Metadata *resolveGenericParamRef(
                            const GenericParamRef &param,
                            SubstFlatGenericParameterFn substFlatGenericParam) {
  // Resolve the root generic parameter.
  const Metadata *current = substFlatGenericParam(param.getRootParamIndex());
  if (!current) return nullptr;

  // Follow the associated type path.
  for (const auto &assocTypeRef : param) {
    // Look for the witness table.
    auto witnessTable =
      swift_conformsToProtocol(current, assocTypeRef.Protocol);
    if (!witnessTable) return nullptr;

    // Call the associated type access function.
    using AssociatedTypeAccessFn =
      const Metadata *(*)(const Metadata *base, const WitnessTable *);
    unsigned adjustedIndex =
      assocTypeRef.Index + WitnessTableFirstRequirementOffset;
    current =
      ((const AssociatedTypeAccessFn *)witnessTable)[adjustedIndex]
        (current, witnessTable);
    if (!current) return nullptr;
  }

  return current;
}

bool swift::_checkGenericRequirements(
                      llvm::ArrayRef<GenericRequirementDescriptor> requirements,
                      std::vector<const void *> &extraArguments,
                      SubstFlatGenericParameterFn substFlatGenericParam,
                      SubstGenericParameterFn substGenericParam) {
  for (const auto &req : requirements) {
    // Make sure we understand the requirement we're dealing with.
    switch (req.getKind()) {
    case GenericRequirementKind::BaseClass:
    case GenericRequirementKind::Layout:
    case GenericRequirementKind::Protocol:
    case GenericRequirementKind::SameConformance:
    case GenericRequirementKind::SameType:
      break;

    default:
      // Unknown requirement kind. Bail out.
      return true;
    }

    // Resolve the subject generic parameter.
    auto subjectType =
      resolveGenericParamRef(req.getParam(), substFlatGenericParam);
    if (!subjectType) return true;

    // Check the requirement.
    switch (req.getKind()) {
    case GenericRequirementKind::Protocol: {
      // Look for a witness table to satisfy this conformance.
      auto witnessTable =
        swift_conformsToProtocol(subjectType, req.getProtocol());
      if (!witnessTable) return true;

      // If this requirement provides an extra argument, add the witness table
      // as that argument.
      if (req.getFlags().hasExtraArgument())
        extraArguments.push_back(witnessTable);

      continue;
    }

    case GenericRequirementKind::SameType: {
      // Demangle the second type under the given substitutions.
      auto otherType =
        _getTypeByMangledName(req.getMangledTypeName(), substGenericParam);
      if (!otherType) return true;

      assert(!req.getFlags().hasExtraArgument());

      // Check that the types are equivalent.
      if (subjectType != otherType) return true;

      continue;
    }

    case GenericRequirementKind::Layout: {
      switch (req.getLayout()) {
      case GenericRequirementLayoutKind::Class:
        // Check whether the subject type is a class.
        if (!subjectType->isAnyClass()) return true;
        continue;
      }

      // Unknown layout.
      return true;
    }

    case GenericRequirementKind::BaseClass: {
      // Demangle the base type under the given substitutions.
      auto baseType =
        _getTypeByMangledName(req.getMangledTypeName(), substGenericParam);
      if (!baseType) return true;

      // Check whether it's dynamically castable, which works as a superclass
      // check.
      if (!swift_dynamicCastMetatype(subjectType, baseType)) return true;

      continue;
    }

    case GenericRequirementKind::SameConformance: {
      // FIXME: Implement this check.
      continue;
    }
    }

    // Unknown generic requirement kind.
    return true;
  }

  // Success!
  return false;
}
