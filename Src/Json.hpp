#pragma once

#include <string>
#include <vector>

struct JsonMember;

/**
 * @brief A minimal JSON document tree. Object members keep their original order and numbers keep their original text,
 * so a parse/write round trip leaves untouched values exactly as they were.
 */
struct JsonValue
{
	enum class eType
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	eType Type = eType::Null;
	bool Bool = false;
	std::string Text; // string contents, or a number as written
	std::vector<JsonValue> Array;
	std::vector<JsonMember> Object;

	static JsonValue MakeNumber(long long value);
	static JsonValue MakeString(const std::string& value);
	static JsonValue MakeArray();
	static JsonValue MakeObject();

	bool IsObject() const { return Type == eType::Object; }
	bool IsArray() const { return Type == eType::Array; }
	bool IsString() const { return Type == eType::String; }
	bool IsNumber() const { return Type == eType::Number; }

	// Returns the member `key` of an object, or nullptr if missing (or this is not an object).
	JsonValue* Find(const std::string& key);
	const JsonValue* Find(const std::string& key) const;

	// Returns the member `key`, adding it as null if missing. A null value becomes an empty object first.
	JsonValue& operator[](const std::string& key);

	// The number as an integer, or `fallback` if this is not a number.
	long long AsInt(long long fallback = -1) const;
};

struct JsonMember
{
	std::string Key;
	JsonValue Value;
};

// Returns false and sets `error` if `text` is not valid JSON.
bool ParseJson(const std::string& text, JsonValue& out, std::string& error);

std::string WriteJson(const JsonValue& value, bool pretty);
