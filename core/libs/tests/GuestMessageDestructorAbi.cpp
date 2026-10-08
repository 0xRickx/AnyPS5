#include "prx/libc/include/exceptions/Runtime.hpp"
#include <cstdio>
#include <cstring>
#include <regex>
#include <windows.h>

using GuestWhat = const char* (APS5_VABI *)(const void*);
using GuestDestroy = void (APS5_VABI *)(void*);
using HostDestroy = void (*)(void*);
using GuestThrowFunction = void (APS5_VABI *)(std::uintptr_t);
struct TypeRecord { const void* vtable; const char* name; const TypeRecord* base; };
struct Object { const void* vtable; const char* message; };
struct Table { std::ptrdiff_t offset; const TypeRecord* type; GuestDestroy destroy; GuestDestroy deleteObject; GuestWhat what; };
struct Message { std::size_t length; std::size_t capacity; std::atomic<std::ptrdiff_t> references; };
static_assert(sizeof(Object) == 16);

extern "C" {
extern const unsigned char _ZTVN10__cxxabiv117__class_type_infoE_nid_postfix[];
extern const unsigned char _ZTVN10__cxxabiv120__si_class_type_infoE_nid_postfix[];
extern const unsigned char _ZTISt12out_of_range_nid_postfix[];
extern const unsigned char _ZTISt11regex_error_nid_postfix[];
void APS5_VABI _ZNSt12out_of_rangeC1EPKc_nid_postfix(Object*, const char*);
void APS5_VABI _ZNSt12out_of_rangeC1ERKS__nid_postfix(Object*, const Object*);
void APS5_VABI _ZNSt12out_of_rangeD1Ev_nid_postfix(Object*);
[[noreturn]] void APS5_VABI _ZSt14_Xout_of_rangePKc_nid_postfix(const char*);
[[noreturn]] void APS5_VABI _ZSt13_Xregex_errorNSt15regex_constants10error_typeE_nid_postfix(std::regex_constants::error_type);
int APS5_VABI GuestOuter();
void APS5_VABI GuestStdCapture();
void APS5_VABI ProbeDestroy(GuestDestroy, void*, void*);

TypeRecord FixtureBaseType{_ZTVN10__cxxabiv117__class_type_infoE_nid_postfix + 16, "11FixtureBase", nullptr};
TypeRecord FixtureErrorType{_ZTVN10__cxxabiv120__si_class_type_infoE_nid_postfix + 16, "12FixtureError", &FixtureBaseType};
TypeRecord FixtureOtherType{_ZTVN10__cxxabiv117__class_type_infoE_nid_postfix + 16, "12FixtureOther", nullptr};
const void* FixtureStdOutType = _ZTISt12out_of_range_nid_postfix;
GuestThrowFunction FixtureThrow = reinterpret_cast<GuestThrowFunction>(_ZSt14_Xout_of_rangePKc_nid_postfix);
std::uintptr_t FixtureArgument;
extern const char FixtureMessage[] = "own System-V guest message";
void* FixtureLastObject = nullptr;
unsigned FixtureDestroyed = 0;
unsigned FixtureGuardDestroyed = 0;
unsigned FixtureCaught = 0;
unsigned FixtureWhatChecked = 0;

[[noreturn]] void APS5_VABI FixtureFail(unsigned code) {
    std::fprintf(stderr, "guest exception failure %u\n", code);
    ExitProcess(code);
}
const char* APS5_VABI FixtureWhat(const void* object) { return static_cast<const Object*>(object)->message; }
void APS5_VABI FixtureDestroy(void* object) {
    if (object != FixtureLastObject) FixtureFail(11);
    ++FixtureDestroyed;
}
Table FixtureVtable{0, &FixtureErrorType, FixtureDestroy, FixtureDestroy, FixtureWhat};
void APS5_VABI FixtureCheckWhat(const char* message) {
    if (!message || std::strcmp(message, FixtureMessage)) FixtureFail(12);
    ++FixtureWhatChecked;
}
}

static Object decoy{}, retained{};
static GuestDestroy registeredDestructor;
static void* expectedObject;
static void* poison;
static unsigned callbacks;
static bool regularRelease;
static bool keepMessage;

static Message* MessageHeader(const Object& object) {
    return reinterpret_cast<Message*>(const_cast<char*>(object.message)) - 1;
}

extern "C" void APS5_VABI ObserveDestroy(void* pointer) {
    if (pointer != expectedObject || ++callbacks != 1) FixtureFail(40);
    ProbeDestroy(registeredDestructor, pointer, poison);
    if (static_cast<Object*>(pointer)->message != nullptr ||
        !decoy.message || std::strcmp(decoy.message, "unrelated message") ||
        (keepMessage && MessageHeader(retained)->references.load() != 0)) FixtureFail(41);
}

extern "C" void APS5_VABI FixtureInspect(void* pointer) {
    auto* header = LibcException::FromObject(pointer);
    if (header->_pad != 0 || !header->destructor) FixtureFail(42);
    expectedObject = pointer;
    if (keepMessage) {
        _ZNSt12out_of_rangeC1ERKS__nid_postfix(&retained, static_cast<Object*>(pointer));
        if (MessageHeader(retained)->references.load() != 1) FixtureFail(43);
    }
    registeredDestructor = reinterpret_cast<GuestDestroy>(header->destructor);
    if (!regularRelease) header->destructor = reinterpret_cast<HostDestroy>(ObserveDestroy);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "control") == 0) {
        if (GuestOuter() != 0 || FixtureDestroyed != 1 || FixtureGuardDestroyed != 1 ||
            FixtureCaught != 2 || FixtureWhatChecked != 2) return 10;
        return 0;
    }
    const bool regex = std::strstr(argv[1], "regex") != nullptr;
    regularRelease = std::strcmp(argv[1], "release") == 0;
    keepMessage = std::strstr(argv[1], "unshared") == nullptr;
    FixtureArgument = reinterpret_cast<std::uintptr_t>(FixtureMessage);
    if (regex) {
        FixtureStdOutType = _ZTISt11regex_error_nid_postfix;
        FixtureThrow = reinterpret_cast<GuestThrowFunction>(_ZSt13_Xregex_errorNSt15regex_constants10error_typeE_nid_postfix);
        FixtureArgument = static_cast<std::uintptr_t>(std::regex_constants::error_collate);
    }
    _ZNSt12out_of_rangeC1EPKc_nid_postfix(&decoy, "unrelated message");
    poison = std::strstr(argv[1], "zero") ? nullptr : &decoy;
    GuestStdCapture();
    if (callbacks != (regularRelease ? 0u : 1u) ||
        (keepMessage && (MessageHeader(retained)->references.load() != 0 ||
            std::strcmp(retained.message, regex ? "regular expression error" : FixtureMessage))) ||
        !decoy.message || std::strcmp(decoy.message, "unrelated message")) return 44;
    _ZNSt12out_of_rangeD1Ev_nid_postfix(&retained);
    _ZNSt12out_of_rangeD1Ev_nid_postfix(&decoy);
    if (retained.message || decoy.message) return 45;
    std::puts("guest message callback, retained ownership and release: PASS");
    return 0;
}
