/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "support/protobuf.h"

namespace PCSX::Protobuf {

enum class WireError {
    None,
    TooLarge,
    TruncatedVarint,
    VarintOverflow,
    UInt32Overflow,
    InvalidTag,
    UnknownField,
    DuplicateField,
    IncompatibleWire,
    TruncatedScalar,
    TruncatedLength,
    InvalidBoolean,
    InvalidInt32,
    ValueLimit,
    DepthLimit,
};

struct WireValidationResult {
    WireError error = WireError::None;
    const char* messageType = nullptr;
    const char* fieldName = nullptr;
    uint32_t fieldNumber = 0;

    explicit operator bool() const { return error == WireError::None; }
    const char* message() const {
        switch (error) {
            case WireError::None: return "";
            case WireError::TooLarge: return "state exceeds 64 MiB";
            case WireError::TruncatedVarint: return "truncated varint";
            case WireError::VarintOverflow: return "varint overflow";
            case WireError::UInt32Overflow: return "32-bit varint overflow";
            case WireError::InvalidTag: return "invalid state field tag";
            case WireError::UnknownField: return "unknown encoded field";
            case WireError::DuplicateField: return "duplicate singular field";
            case WireError::IncompatibleWire: return "incompatible wire type";
            case WireError::TruncatedScalar: return "truncated scalar field";
            case WireError::TruncatedLength: return "truncated length-delimited field";
            case WireError::InvalidBoolean: return "invalid encoded boolean";
            case WireError::InvalidInt32: return "invalid encoded int32 sign extension";
            case WireError::ValueLimit: return "state encoded value limit exceeded";
            case WireError::DepthLimit: return "state schema nesting exceeds 64";
        }
        return "invalid state protobuf";
    }
};

namespace WireDetail {

template <typename Field>
struct ValueType;
template <typename Value, typename Name, uint64_t Number>
struct ValueType<Field<Value, Name, Number>> { using type = Value; };
template <typename Value, typename Name, uint64_t Number>
struct ValueType<FieldRef<Value, Name, Number>> { using type = Value; };
template <typename Value, typename Name, uint64_t Number>
struct ValueType<FieldPtr<Value, Name, Number>> { using type = Value; };
template <typename Value, size_t Size, typename Name, uint64_t Number>
struct ValueType<RepeatedField<Value, Size, Name, Number>> { using type = Value; };
template <typename Value, size_t Size, typename Name, uint64_t Number>
struct ValueType<RepeatedFieldRef<Value, Size, Name, Number>> { using type = Value; };
template <typename Value, typename Name, uint64_t Number>
struct ValueType<RepeatedVariableField<Value, Name, Number>> { using type = Value; };
template <typename Value, typename Name, uint64_t Number>
struct ValueType<MessageField<Value, Name, Number>> { using type = Value; };

template <typename Value, typename = void>
struct IsMessage : std::false_type {};
template <typename Value>
struct IsMessage<Value, std::void_t<decltype(Value::isMessage)>> : std::bool_constant<Value::isMessage> {};

struct Slice {
    const uint8_t* bytes;
    size_t size;
    size_t position = 0;

    size_t remaining() const { return size - position; }
};

struct Context {
    WireValidationResult result;
    size_t values = 0;
    bool strict = true;

    bool fail(WireError error) {
        result.error = error;
        return false;
    }
    bool identify(const char* messageType, const char* fieldName = nullptr, uint32_t number = 0) {
        if (!result.messageType) {
            result.messageType = messageType;
            result.fieldName = fieldName;
            result.fieldNumber = number;
        }
        return false;
    }
    bool count() { return ++values <= 100000 || fail(WireError::ValueLimit); }
    bool varint(Slice& slice, uint64_t& value, bool narrow = false) {
        value = 0;
        for (unsigned index = 0; index < 10; ++index) {
            if (!slice.remaining()) return fail(WireError::TruncatedVarint);
            const uint8_t byte = slice.bytes[slice.position++];
            if (index == 9 && (strict ? byte > 1 : byte & 0x80)) return fail(WireError::VarintOverflow);
            value |= uint64_t(byte & 0x7f) << (index * 7);
            if (byte & 0x80) continue;
            if (narrow && value > UINT32_MAX) return fail(WireError::UInt32Overflow);
            return true;
        }
        return fail(WireError::VarintOverflow);
    }
    bool skip(Slice& slice, unsigned wire, uint32_t number, unsigned depth) {
        uint64_t value;
        if (wire == 0) return count() && varint(slice, value);
        if (wire == 1 || wire == 5) {
            const size_t size = wire == 1 ? 8 : 4;
            if (slice.remaining() < size) return fail(WireError::TruncatedScalar);
            slice.position += size;
            return count();
        }
        if (wire == 2) {
            if (!varint(slice, value)) return false;
            if (value > slice.remaining()) return fail(WireError::TruncatedLength);
            slice.position += size_t(value);
            return true;
        }
        if (wire != 3) return fail(WireError::InvalidTag);
        if (depth >= 64) return fail(WireError::DepthLimit);
        while (slice.remaining()) {
            if (!count() || !varint(slice, value)) return false;
            const uint32_t tag = uint32_t(value);
            if ((tag & 7) == 4) return tag >> 3 == number || fail(WireError::InvalidTag);
            if (tag < 8) return fail(WireError::InvalidTag);
            if (!skip(slice, tag & 7, tag >> 3, depth + 1)) return false;
        }
        return fail(WireError::TruncatedLength);
    }
};

template <typename MessageType>
struct Schema;

template <typename Value>
bool scalar(Context& context, Slice& slice) {
    if (!context.count()) return false;
    if constexpr (Value::wireType == 1 || Value::wireType == 5) {
        constexpr size_t size = Value::wireType == 1 ? 8 : 4;
        if (slice.remaining() < size) return context.fail(WireError::TruncatedScalar);
        slice.position += size;
        return true;
    } else {
        static_assert(Value::wireType == 0);
        constexpr std::string_view kind = Value::typeName;
        uint64_t value;
        if (!context.varint(slice, value, context.strict && (kind == "uint32" || kind == "sint32"))) return false;
        if constexpr (kind == "bool") {
            if (context.strict && value > 1) return context.fail(WireError::InvalidBoolean);
        }
        if constexpr (kind == "int32") {
            if (context.strict && value > INT32_MAX && value < UINT64_MAX - INT32_MAX)
                return context.fail(WireError::InvalidInt32);
        }
        return true;
    }
}

template <typename Value>
bool field(Context& context, Slice& slice, unsigned wire, unsigned depth) {
    if (wire != 2) {
        if constexpr (Value::wireType != 2) return scalar<Value>(context, slice);
        return context.fail(WireError::IncompatibleWire);
    }
    uint64_t length;
    if (!context.varint(slice, length, context.strict)) return false;
    if (length > slice.remaining()) return context.fail(WireError::TruncatedLength);
    Slice nested{slice.bytes + slice.position, size_t(length)};
    slice.position += size_t(length);
    if constexpr (IsMessage<Value>::value) {
        return Schema<Value>::read(context, nested, depth + 1);
    } else if constexpr (Value::wireType != 2) {
        while (nested.remaining()) {
            if (!scalar<Value>(context, nested)) return false;
        }
    }
    return true;
}

struct SchemaView;

struct Descriptor {
    uint32_t number;
    unsigned wire;
    bool repeated;
    const char* name;
    const char* kind;
    bool (*read)(Context&, Slice&, unsigned, unsigned);
    const SchemaView* (*child)();
};

struct SchemaView {
    const char* name;
    const Descriptor* fields;
    size_t size;

    const Descriptor* find(std::string_view name) const {
        for (size_t index = 0; index < size; ++index) {
            if (name == fields[index].name) return fields + index;
        }
        return nullptr;
    }
};

template <typename Value>
const SchemaView* childSchema() {
    if constexpr (IsMessage<Value>::value) return &Schema<Value>::view;
    return nullptr;
}

template <typename Name, typename... Fields>
struct Schema<Message<Name, Fields...>> {
    inline static constexpr std::array<Descriptor, sizeof...(Fields)> descriptors{{
        {uint32_t(Fields::fieldNumber), ValueType<Fields>::type::wireType, Fields::repeated,
         Fields::fieldName::data(), ValueType<Fields>::type::typeName,
         &field<typename ValueType<Fields>::type>, &childSchema<typename ValueType<Fields>::type>}...
    }};

    static bool read(Context& context, Slice& slice, unsigned depth) {
        if (depth > 64) {
            context.fail(WireError::DepthLimit);
            return context.identify(Name::data());
        }
        std::array<bool, sizeof...(Fields)> seen{};
        while (slice.remaining()) {
            uint64_t tag;
            if (!context.count() || !context.varint(slice, tag, context.strict)) return context.identify(Name::data());
            if (!context.strict) tag = uint32_t(tag);
            if (tag < 8) {
                context.fail(WireError::InvalidTag);
                return context.identify(Name::data());
            }
            const uint32_t number = tag >> 3;
            const unsigned wire = tag & 7;
            size_t index = 0;
            while (index < descriptors.size() && descriptors[index].number != number) ++index;
            if (index == descriptors.size()) {
                if (!context.strict) {
                    if (!context.skip(slice, wire, number, depth)) return context.identify(Name::data(), nullptr, number);
                    continue;
                }
                context.fail(WireError::UnknownField);
                return context.identify(Name::data(), nullptr, number);
            }
            const auto& descriptor = descriptors[index];
            if (context.strict && !descriptor.repeated && seen[index]) {
                context.fail(WireError::DuplicateField);
                return context.identify(Name::data(), descriptor.name, number);
            }
            seen[index] = true;
            if (wire != descriptor.wire && !(descriptor.repeated && wire == 2 && descriptor.wire != 2)) {
                context.fail(WireError::IncompatibleWire);
                return context.identify(Name::data(), descriptor.name, number);
            }
            if (!descriptor.read(context, slice, wire, depth)) {
                return context.identify(Name::data(), descriptor.name, number);
            }
        }
        return true;
    }

    inline static constexpr SchemaView view{Name::data(), descriptors.data(), descriptors.size()};
};

template <typename Name>
struct Schema<EmptyMessage<Name>> : Schema<Message<Name>> {};

}

template <typename MessageType>
struct StrictWireValidator {
    inline static constexpr size_t MaxSize = 67108864;

    static WireValidationResult validate(const void* bytes, size_t size) {
        if (size > MaxSize) return {WireError::TooLarge};
        WireDetail::Context context;
        WireDetail::Slice slice{static_cast<const uint8_t*>(bytes), size};
        WireDetail::Schema<MessageType>::read(context, slice, 0);
        return context.result;
    }
};

}
