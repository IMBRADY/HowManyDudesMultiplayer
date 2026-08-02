// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Minimal self-contained JSON value / parser / writer.
//
// Deliberately dependency-free: the mod ships as a single DLL injected into a
// shipped game, so pulling in a third-party JSON library would only add build
// surface and another thing to go wrong at load time. This covers exactly the
// subset the wire protocol needs.
//
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hmd::json
{
	enum class Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	class Value
	{
	public:
		Value() = default;
		Value(bool Boolean) : m_Type(Type::Bool), m_Bool(Boolean) {}
		Value(double Number) : m_Type(Type::Number), m_Number(Number) {}
		Value(int Number) : m_Type(Type::Number), m_Number(static_cast<double>(Number)) {}
		Value(const char* String) : m_Type(Type::String), m_String(String ? String : "") {}
		Value(std::string String) : m_Type(Type::String), m_String(std::move(String)) {}

		static Value Array() { Value v; v.m_Type = Type::Array; return v; }
		static Value Object() { Value v; v.m_Type = Type::Object; return v; }

		Type GetType() const { return m_Type; }
		bool IsNull() const { return m_Type == Type::Null; }
		bool IsObject() const { return m_Type == Type::Object; }
		bool IsArray() const { return m_Type == Type::Array; }

		// --- Accessors. All of these are total: a type mismatch or a missing
		// --- key yields the supplied fallback rather than throwing, because a
		// --- malformed peer payload must never take the game down.
		bool AsBool(bool Fallback = false) const
		{
			if (m_Type == Type::Bool) return m_Bool;
			if (m_Type == Type::Number) return m_Number != 0.0;
			return Fallback;
		}

		double AsNumber(double Fallback = 0.0) const
		{
			if (m_Type == Type::Number) return m_Number;
			if (m_Type == Type::Bool) return m_Bool ? 1.0 : 0.0;
			return Fallback;
		}

		int AsInt(int Fallback = 0) const
		{
			return m_Type == Type::Number ? static_cast<int>(m_Number) : Fallback;
		}

		std::string AsString(const std::string& Fallback = {}) const
		{
			return m_Type == Type::String ? m_String : Fallback;
		}

		const std::vector<Value>& Items() const { return m_Array; }
		size_t Size() const { return m_Type == Type::Array ? m_Array.size() : 0; }

		// Object members, for callers that need to inspect a document's shape
		// rather than look up known keys. Empty for every non-object value.
		const std::map<std::string, Value>& Members() const { return m_Object; }

		bool Has(const std::string& Key) const
		{
			return m_Type == Type::Object && m_Object.find(Key) != m_Object.end();
		}

		const Value& operator[](const std::string& Key) const
		{
			static const Value kNull;
			if (m_Type != Type::Object) return kNull;
			auto it = m_Object.find(Key);
			return it == m_Object.end() ? kNull : it->second;
		}

		// --- Builders.
		void Set(const std::string& Key, Value Item)
		{
			m_Type = Type::Object;
			m_Object[Key] = std::move(Item);
		}

		void Push(Value Item)
		{
			m_Type = Type::Array;
			m_Array.push_back(std::move(Item));
		}

		std::string Serialize() const
		{
			std::string out;
			Write(out);
			return out;
		}

	private:
		void Write(std::string& Out) const;

		Type m_Type = Type::Null;
		bool m_Bool = false;
		double m_Number = 0.0;
		std::string m_String;
		std::vector<Value> m_Array;
		std::map<std::string, Value> m_Object;
	};

	// Returns false (and leaves Out untouched) on malformed input.
	bool Parse(const std::string& Text, Value& Out);
}
