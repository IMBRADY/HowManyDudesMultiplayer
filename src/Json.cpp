// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Json.h"

#include <cmath>
#include <cstdio>

namespace hmd::json
{
	namespace
	{
		void WriteEscaped(const std::string& Text, std::string& Out)
		{
			Out.push_back('"');
			for (unsigned char c : Text)
			{
				switch (c)
				{
				case '"':  Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\b': Out += "\\b";  break;
				case '\f': Out += "\\f";  break;
				case '\n': Out += "\\n";  break;
				case '\r': Out += "\\r";  break;
				case '\t': Out += "\\t";  break;
				default:
					if (c < 0x20)
					{
						char buffer[8]{};
						_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "\\u%04x", c);
						Out += buffer;
					}
					else
					{
						Out.push_back(static_cast<char>(c));
					}
					break;
				}
			}
			Out.push_back('"');
		}

		// --- Parser -------------------------------------------------------
		struct Parser
		{
			const std::string& text;
			size_t pos = 0;
			// Guards against a hostile or corrupt payload nesting deeply enough
			// to blow the stack of whichever thread is decoding it.
			int depth = 0;
			static constexpr int kMaxDepth = 64;

			explicit Parser(const std::string& Text) : text(Text) {}

			void SkipWhitespace()
			{
				while (pos < text.size())
				{
					char c = text[pos];
					if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
					else break;
				}
			}

			bool Literal(const char* Word)
			{
				size_t length = strlen(Word);
				if (text.compare(pos, length, Word) != 0)
					return false;
				pos += length;
				return true;
			}

			bool ParseString(std::string& Out)
			{
				if (pos >= text.size() || text[pos] != '"')
					return false;
				pos++;

				while (pos < text.size())
				{
					char c = text[pos++];
					if (c == '"')
						return true;

					if (c != '\\')
					{
						Out.push_back(c);
						continue;
					}

					if (pos >= text.size())
						return false;

					char escape = text[pos++];
					switch (escape)
					{
					case '"':  Out.push_back('"');  break;
					case '\\': Out.push_back('\\'); break;
					case '/':  Out.push_back('/');  break;
					case 'b':  Out.push_back('\b'); break;
					case 'f':  Out.push_back('\f'); break;
					case 'n':  Out.push_back('\n'); break;
					case 'r':  Out.push_back('\r'); break;
					case 't':  Out.push_back('\t'); break;
					case 'u':
					{
						if (pos + 4 > text.size())
							return false;

						unsigned int code = 0;
						for (int i = 0; i < 4; i++)
						{
							char h = text[pos + i];
							code <<= 4;
							if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
							else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
							else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
							else return false;
						}
						pos += 4;

						// Encode as UTF-8. Surrogate pairs are passed through as
						// replacement characters; the payloads this mod exchanges
						// are ASCII identifiers and numbers.
						if (code < 0x80)
						{
							Out.push_back(static_cast<char>(code));
						}
						else if (code < 0x800)
						{
							Out.push_back(static_cast<char>(0xC0 | (code >> 6)));
							Out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
						}
						else
						{
							Out.push_back(static_cast<char>(0xE0 | (code >> 12)));
							Out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
							Out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
						}
						break;
					}
					default:
						return false;
					}
				}
				return false;
			}

			bool ParseValue(Value& Out)
			{
				if (depth >= kMaxDepth)
					return false;

				SkipWhitespace();
				if (pos >= text.size())
					return false;

				char c = text[pos];

				if (c == 'n')
				{
					if (!Literal("null")) return false;
					Out = Value();
					return true;
				}
				if (c == 't')
				{
					if (!Literal("true")) return false;
					Out = Value(true);
					return true;
				}
				if (c == 'f')
				{
					if (!Literal("false")) return false;
					Out = Value(false);
					return true;
				}
				if (c == '"')
				{
					std::string s;
					if (!ParseString(s)) return false;
					Out = Value(std::move(s));
					return true;
				}
				if (c == '[')
				{
					pos++;
					depth++;
					Out = Value::Array();
					SkipWhitespace();
					if (pos < text.size() && text[pos] == ']') { pos++; depth--; return true; }

					while (true)
					{
						Value element;
						if (!ParseValue(element)) return false;
						Out.Push(std::move(element));

						SkipWhitespace();
						if (pos >= text.size()) return false;
						if (text[pos] == ',') { pos++; continue; }
						if (text[pos] == ']') { pos++; depth--; return true; }
						return false;
					}
				}
				if (c == '{')
				{
					pos++;
					depth++;
					Out = Value::Object();
					SkipWhitespace();
					if (pos < text.size() && text[pos] == '}') { pos++; depth--; return true; }

					while (true)
					{
						SkipWhitespace();
						std::string key;
						if (!ParseString(key)) return false;

						SkipWhitespace();
						if (pos >= text.size() || text[pos] != ':') return false;
						pos++;

						Value element;
						if (!ParseValue(element)) return false;
						Out.Set(key, std::move(element));

						SkipWhitespace();
						if (pos >= text.size()) return false;
						if (text[pos] == ',') { pos++; continue; }
						if (text[pos] == '}') { pos++; depth--; return true; }
						return false;
					}
				}

				// Number.
				size_t start = pos;
				if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) pos++;
				bool digits = false;
				while (pos < text.size())
				{
					char d = text[pos];
					if ((d >= '0' && d <= '9')) { digits = true; pos++; }
					else if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') pos++;
					else break;
				}
				if (!digits)
					return false;

				Out = Value(strtod(text.substr(start, pos - start).c_str(), nullptr));
				return true;
			}
		};
	}

	void Value::Write(std::string& Out) const
	{
		switch (m_Type)
		{
		case Type::Null:
			Out += "null";
			break;

		case Type::Bool:
			Out += m_Bool ? "true" : "false";
			break;

		case Type::Number:
		{
			char buffer[64]{};
			// Integral values are emitted without a decimal tail so the payload
			// stays readable and stable across round-trips.
			if (std::isfinite(m_Number) && m_Number == std::floor(m_Number) &&
				std::fabs(m_Number) < 1e15)
			{
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%lld",
					static_cast<long long>(m_Number));
			}
			else if (std::isfinite(m_Number))
			{
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%.10g", m_Number);
			}
			else
			{
				// JSON has no representation for NaN/inf; emit 0 rather than
				// producing a document the peer cannot parse.
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0");
			}
			Out += buffer;
			break;
		}

		case Type::String:
			WriteEscaped(m_String, Out);
			break;

		case Type::Array:
		{
			Out.push_back('[');
			for (size_t i = 0; i < m_Array.size(); i++)
			{
				if (i) Out.push_back(',');
				m_Array[i].Write(Out);
			}
			Out.push_back(']');
			break;
		}

		case Type::Object:
		{
			Out.push_back('{');
			bool first = true;
			for (const auto& [key, item] : m_Object)
			{
				if (!first) Out.push_back(',');
				first = false;
				WriteEscaped(key, Out);
				Out.push_back(':');
				item.Write(Out);
			}
			Out.push_back('}');
			break;
		}
		}
	}

	bool Parse(const std::string& Text, Value& Out)
	{
		Parser parser(Text);
		Value parsed;
		if (!parser.ParseValue(parsed))
			return false;

		parser.SkipWhitespace();
		if (parser.pos != Text.size())
			return false;

		Out = std::move(parsed);
		return true;
	}
}
