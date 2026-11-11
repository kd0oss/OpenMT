/* OpenMT Radio: a small JSON value for the remote control protocol (parse and write; no dependencies). */

#ifndef OPENMT_REMOTE_JSON_H
#define OPENMT_REMOTE_JSON_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace openmt
{

class Json
{
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Null), bool_(false), num_(0.0) {}
    Json(bool v) : type_(Bool), bool_(v), num_(0.0) {}
    Json(int v) : type_(Number), bool_(false), num_((double)v) {}
    Json(unsigned int v) : type_(Number), bool_(false), num_((double)v) {}
    Json(long long v) : type_(Number), bool_(false), num_((double)v) {}
    Json(unsigned long long v) : type_(Number), bool_(false), num_((double)v) {}
    Json(double v) : type_(Number), bool_(false), num_(v) {}
    Json(const char* v) : type_(String), bool_(false), num_(0.0), str_(v) {}
    Json(const std::string& v) : type_(String), bool_(false), num_(0.0), str_(v) {}

    static Json object() { Json j; j.type_ = Object; return j; }
    static Json array() { Json j; j.type_ = Array; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isBool() const { return type_ == Bool; }
    bool isNumber() const { return type_ == Number; }
    bool isString() const { return type_ == String; }
    bool isArray() const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }

    /* getters with a fallback for the wrong type or a missing member */
    bool asBool(bool fallback = false) const { return type_ == Bool ? bool_ : fallback; }
    double asNumber(double fallback = 0.0) const { return type_ == Number ? num_ : fallback; }
    long long asInt(long long fallback = 0) const { return type_ == Number ? (long long)num_ : fallback; }
    const std::string& asString() const { return str_; }               /* empty unless a string */
    std::string asString(const std::string& fallback) const { return type_ == String ? str_ : fallback; }

    /* object members */
    bool has(const std::string& key) const { return type_ == Object && obj_.find(key) != obj_.end(); }
    const Json& operator[](const std::string& key) const;              /* a null Json when missing */
    Json& set(const std::string& key, const Json& value);              /* turns a null into an object */
    const std::map<std::string, Json>& members() const { return obj_; }

    /* array items */
    size_t size() const { return type_ == Array ? arr_.size() : 0U; }
    const Json& at(size_t i) const;
    Json& push(const Json& value);                                     /* turns a null into an array */
    const std::vector<Json>& items() const { return arr_; }

    /* The text of the value (UTF-8, no white space) */
    std::string dump() const;

    /* Parses a complete document.  false (and an error text) on bad syntax, nesting deeper than 16 or trailing characters. */
    static bool parse(const std::string& text, Json& out, std::string& error);

private:
    void dumpTo(std::string& out) const;

    Type                        type_;
    bool                        bool_;
    double                      num_;
    std::string                 str_;
    std::vector<Json>           arr_;
    std::map<std::string, Json> obj_;
};

}

#endif
