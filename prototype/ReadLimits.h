#pragma once

#include <cstdint>

enum class ReadFailure { None, InvalidInput, ResourceLimit, UnsupportedKdf, IoError, UnsupportedData,
                         Cancelled, Timeout, WorkerFailure, RecoveryFailure,
                         KeyFileIoError, InvalidKeyFile, KeyFileTooLarge };

// Provisional development policy, not KDBX format limits. No UI bypass is
// provided. Tests may use smaller budgets to exercise exact boundaries.
struct ReadLimits
{
    std::uint64_t fileBytes = 128ULL << 20;
    std::uint64_t keyFileBytes = 16ULL << 20;
    std::uint64_t headerBytes = 1ULL << 20;
    std::uint64_t headerFields = 64;
    std::uint64_t outerFieldBytes = 1ULL << 20;
    std::uint64_t dictionaryEntries = 64;
    std::uint64_t dictionaryNameBytes = 1024;
    std::uint64_t innerHeaderBytes = 64ULL << 20;
    std::uint64_t innerFields = 4096;
    std::uint64_t innerFieldBytes = 16ULL << 20;
    std::uint64_t blockBytes = 16ULL << 20;
    std::uint64_t expandedBytes = 256ULL << 20;
    std::uint64_t aesRounds = 100000000;
    std::uint64_t argonMemoryKiB = 256ULL << 10;
    std::uint64_t argonRounds = 20;
    std::uint64_t argonLanes = 8;
    std::uint64_t argonWorkKiB = 1024ULL << 10;
    std::uint64_t xmlDepth = 128;
    std::uint64_t xmlElements = 1000000;
    std::uint64_t modelItems = 250000;
    std::uint64_t groupObjects = 20000;
    std::uint64_t entryObjects = 100000;
    std::uint64_t historyItems = 50000;
    std::uint64_t binaryItems = 4096;
    std::uint64_t attachmentReferences = 100000;
    std::uint64_t customDataItems = 100000;
    std::uint64_t customIcons = 4096;
    std::uint64_t deletedObjects = 100000;
    std::uint64_t autoTypeAssociations = 100000;
    std::uint64_t stringFields = 100000;
};

struct ReadStats
{
    std::uint64_t kdfStarts = 0;
    std::uint64_t largestDeclaredRead = 0;
    std::uint64_t expandedBytes = 0;
    std::uint64_t xmlElements = 0;
    std::uint64_t maximumXmlDepth = 0;
    std::uint64_t modelItems = 0;
    std::uint64_t groupObjects = 0;
    std::uint64_t entryObjects = 0;
    std::uint64_t historyItems = 0;
    std::uint64_t binaryItems = 0;
    std::uint64_t attachmentReferences = 0;
    std::uint64_t customDataItems = 0;
    std::uint64_t customIcons = 0;
    std::uint64_t deletedObjects = 0;
    std::uint64_t autoTypeAssociations = 0;
    std::uint64_t stringFields = 0;
};
