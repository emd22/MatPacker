#include "Json.hpp"

#include <cstdlib>

namespace {

class Parser
{
public:
	explicit Parser(const std::string& text) : mText(text) {}

	bool Parse(JsonValue& out, std::string& error)
	{
		SkipSpace();

		if (!ParseValue(out, 0)) {
			error = mError + " at offset " + std::to_string(mPos);
			return false;
		}

		SkipSpace();

		if (mPos != mText.size()) {
			error = "unexpected trailing data at offset " + std::to_string(mPos);
			return false;
		}

		return true;
	}

private:
	static constexpr int spcMaxDepth = 512;

	bool Fail(const char* message)
	{
		mError = message;
		return false;
	}

	void SkipSpace()
	{
		while (mPos < mText.size() &&
			   (mText[mPos] == ' ' || mText[mPos] == '\t' || mText[mPos] == '\n' || mText[mPos] == '\r')) {
			mPos++;
		}
	}

	bool Consume(const char* word)
	{
		const std::string w = word;

		if (mText.compare(mPos, w.size(), w) != 0) {
			return false;
		}

		mPos += w.size();
		return true;
	}

	bool ParseValue(JsonValue& out, int depth)
	{
		if (depth > spcMaxDepth) {
			return Fail("nesting too deep");
		}

		if (mPos >= mText.size()) {
			return Fail("unexpected end of input");
		}

		const char c = mText[mPos];

		if (c == '{') {
			return ParseObject(out, depth);
		}
		if (c == '[') {
			return ParseArray(out, depth);
		}
		if (c == '"') {
			out.Type = JsonValue::eType::String;
			return ParseString(out.Text);
		}
		if (Consume("true")) {
			out.Type = JsonValue::eType::Bool;
			out.Bool = true;
			return true;
		}
		if (Consume("false")) {
			out.Type = JsonValue::eType::Bool;
			out.Bool = false;
			return true;
		}
		if (Consume("null")) {
			out.Type = JsonValue::eType::Null;
			return true;
		}

		return ParseNumber(out);
	}

	bool ParseObject(JsonValue& out, int depth)
	{
		out.Type = JsonValue::eType::Object;
		mPos++; // {
		SkipSpace();

		if (mPos < mText.size() && mText[mPos] == '}') {
			mPos++;
			return true;
		}

		while (true) {
			SkipSpace();

			if (mPos >= mText.size() || mText[mPos] != '"') {
				return Fail("expected a member name");
			}

			JsonMember member;

			if (!ParseString(member.Key)) {
				return false;
			}

			SkipSpace();

			if (mPos >= mText.size() || mText[mPos] != ':') {
				return Fail("expected ':'");
			}

			mPos++;
			SkipSpace();

			if (!ParseValue(member.Value, depth + 1)) {
				return false;
			}

			out.Object.push_back(std::move(member));
			SkipSpace();

			if (mPos < mText.size() && mText[mPos] == ',') {
				mPos++;
				continue;
			}
			if (mPos < mText.size() && mText[mPos] == '}') {
				mPos++;
				return true;
			}

			return Fail("expected ',' or '}'");
		}
	}

	bool ParseArray(JsonValue& out, int depth)
	{
		out.Type = JsonValue::eType::Array;
		mPos++; // [
		SkipSpace();

		if (mPos < mText.size() && mText[mPos] == ']') {
			mPos++;
			return true;
		}

		while (true) {
			SkipSpace();
			out.Array.emplace_back();

			if (!ParseValue(out.Array.back(), depth + 1)) {
				return false;
			}

			SkipSpace();

			if (mPos < mText.size() && mText[mPos] == ',') {
				mPos++;
				continue;
			}
			if (mPos < mText.size() && mText[mPos] == ']') {
				mPos++;
				return true;
			}

			return Fail("expected ',' or ']'");
		}
	}

	bool ParseHex4(unsigned& out)
	{
		if (mPos + 4 > mText.size()) {
			return Fail("truncated \\u escape");
		}

		out = 0;

		for (int i = 0; i < 4; i++) {
			const char h = mText[mPos++];
			out <<= 4;

			if (h >= '0' && h <= '9') {
				out |= unsigned(h - '0');
			}
			else if (h >= 'a' && h <= 'f') {
				out |= unsigned(h - 'a' + 10);
			}
			else if (h >= 'A' && h <= 'F') {
				out |= unsigned(h - 'A' + 10);
			}
			else {
				return Fail("invalid \\u escape");
			}
		}

		return true;
	}

	static void AppendUtf8(std::string& out, unsigned cp)
	{
		if (cp < 0x80) {
			out += char(cp);
		}
		else if (cp < 0x800) {
			out += char(0xC0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000) {
			out += char(0xE0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
		else {
			out += char(0xF0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3F));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
	}

	bool ParseString(std::string& out)
	{
		mPos++; // opening quote

		while (mPos < mText.size()) {
			const char c = mText[mPos++];

			if (c == '"') {
				return true;
			}

			if (c != '\\') {
				out += c;
				continue;
			}

			if (mPos >= mText.size()) {
				break;
			}

			const char e = mText[mPos++];

			switch (e) {
			case '"':
			case '\\':
			case '/':
				out += e;
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				unsigned cp = 0;

				if (!ParseHex4(cp)) {
					return false;
				}

				// A high surrogate followed by a low surrogate encodes one code point above U+FFFF.
				if (cp >= 0xD800 && cp <= 0xDBFF && mText.compare(mPos, 2, "\\u") == 0) {
					const size_t saved = mPos;
					unsigned low = 0;
					mPos += 2;

					if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					}
					else {
						mPos = saved;
					}
				}

				AppendUtf8(out, cp);
				break;
			}
			default:
				return Fail("invalid escape");
			}
		}

		return Fail("unterminated string");
	}

	bool ParseNumber(JsonValue& out)
	{
		const size_t start = mPos;

		while (mPos < mText.size()) {
			const char c = mText[mPos];

			if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
				mPos++;
			}
			else {
				break;
			}
		}

		if (mPos == start) {
			return Fail("unexpected character");
		}

		out.Type = JsonValue::eType::Number;
		out.Text = mText.substr(start, mPos - start);
		return true;
	}

	const std::string& mText;
	size_t mPos = 0;
	std::string mError;
};

void WriteString(std::string& out, const std::string& text)
{
	static const char* spcHex = "0123456789abcdef";

	out += '"';

	for (const char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);

		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += spcHex[c >> 4];
				out += spcHex[c & 0xF];
			}
			else {
				out += ch;
			}
		}
	}

	out += '"';
}

void WriteValue(std::string& out, const JsonValue& value, bool pretty, int indent)
{
	auto NewLine = [&](int level)
	{
		if (pretty) {
			out += '\n';
			out.append(size_t(level) * 2, ' ');
		}
	};

	switch (value.Type) {
	case JsonValue::eType::Null:
		out += "null";
		break;
	case JsonValue::eType::Bool:
		out += value.Bool ? "true" : "false";
		break;
	case JsonValue::eType::Number:
		out += value.Text;
		break;
	case JsonValue::eType::String:
		WriteString(out, value.Text);
		break;
	case JsonValue::eType::Array:
		out += '[';

		for (size_t i = 0; i < value.Array.size(); i++) {
			if (i > 0) {
				out += ',';
			}

			NewLine(indent + 1);
			WriteValue(out, value.Array[i], pretty, indent + 1);
		}

		if (!value.Array.empty()) {
			NewLine(indent);
		}

		out += ']';
		break;
	case JsonValue::eType::Object:
		out += '{';

		for (size_t i = 0; i < value.Object.size(); i++) {
			if (i > 0) {
				out += ',';
			}

			NewLine(indent + 1);
			WriteString(out, value.Object[i].Key);
			out += pretty ? ": " : ":";
			WriteValue(out, value.Object[i].Value, pretty, indent + 1);
		}

		if (!value.Object.empty()) {
			NewLine(indent);
		}

		out += '}';
		break;
	}
}

} // namespace

JsonValue JsonValue::MakeNumber(long long value)
{
	JsonValue v;
	v.Type = eType::Number;
	v.Text = std::to_string(value);
	return v;
}

JsonValue JsonValue::MakeString(const std::string& value)
{
	JsonValue v;
	v.Type = eType::String;
	v.Text = value;
	return v;
}

JsonValue JsonValue::MakeArray()
{
	JsonValue v;
	v.Type = eType::Array;
	return v;
}

JsonValue JsonValue::MakeObject()
{
	JsonValue v;
	v.Type = eType::Object;
	return v;
}

JsonValue* JsonValue::Find(const std::string& key)
{
	if (Type != eType::Object) {
		return nullptr;
	}

	for (JsonMember& member : Object) {
		if (member.Key == key) {
			return &member.Value;
		}
	}

	return nullptr;
}

const JsonValue* JsonValue::Find(const std::string& key) const { return const_cast<JsonValue*>(this)->Find(key); }

JsonValue& JsonValue::operator[](const std::string& key)
{
	if (Type == eType::Null) {
		Type = eType::Object;
	}

	if (JsonValue* existing = Find(key)) {
		return *existing;
	}

	Object.push_back({ key, JsonValue() });
	return Object.back().Value;
}

long long JsonValue::AsInt(long long fallback) const
{
	if (Type != eType::Number) {
		return fallback;
	}

	return std::strtoll(Text.c_str(), nullptr, 10);
}

bool ParseJson(const std::string& text, JsonValue& out, std::string& error)
{
	out = JsonValue();
	return Parser(text).Parse(out, error);
}

std::string WriteJson(const JsonValue& value, bool pretty)
{
	std::string out;
	WriteValue(out, value, pretty, 0);
	return out;
}
