// Record names, the worker-to-supervisor line format and the helpers every worker shares.
#include "performance/Workload.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

namespace workload {

std::vector<std::string> recordNames(const Spec &spec)
{
    const auto *kind = findKind(spec.kind);
    std::vector<std::string> names;
    if (kind && spec.startup && !kind->startupRecord.empty()) names.emplace_back(kind->startupRecord);
    const auto metrics = kind ? kind->metrics(spec.mode) : std::vector<std::string_view>{};
    if (metrics.empty()) names.push_back(spec.name);
    for (const auto metric : metrics) names.push_back(spec.name + '-' + std::string(metric));
    return names;
}

std::string metricOf(const std::string &name, const Spec &spec)
{
    const auto *kind = findKind(spec.kind);
    if (!kind) return spec.mode;
    if (!kind->startupRecord.empty() && name == kind->startupRecord) return "startup";
    for (const auto metric : kind->metrics(spec.mode))
        if (name == spec.name + '-' + std::string(metric)) return std::string(metric);
    return spec.mode;
}

std::string sanitize(std::string text)
{
    for (char &c : text) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    if (text.size() > 240) text.resize(240);
    return text;
}

void emitRecord(const Record &record)
{
    std::cout << "MEASURE\t" << record.kind << '\t' << record.name << '\t' << record.outcome << '\t' << record.bytes
              << '\t' << record.lines << '\t' << record.maxLine << '\t'
              << (record.elapsedMs ? std::to_string(*record.elapsedMs) : std::string("-")) << '\t' << record.rssKb
              << '\t' << sanitize(record.detail) << '\n';
    std::cout.flush();
}

namespace {
std::vector<std::string> splitTab(const std::string &line)
{
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else
            current.push_back(c);
    }
    fields.push_back(current);
    return fields;
}
}

std::optional<Record> parseRecord(const std::string &line)
{
    if (line.rfind("MEASURE\t", 0) != 0) return std::nullopt;
    const auto fields = splitTab(line);
    if (fields.size() != 10) return std::nullopt;
    Record record;
    record.kind = fields[1];
    record.name = fields[2];
    record.outcome = fields[3];
    record.bytes = std::strtoull(fields[4].c_str(), nullptr, 10);
    record.lines = std::strtoull(fields[5].c_str(), nullptr, 10);
    record.maxLine = std::strtoull(fields[6].c_str(), nullptr, 10);
    if (fields[7] != "-") record.elapsedMs = std::strtoull(fields[7].c_str(), nullptr, 10);
    record.rssKb = std::strtoull(fields[8].c_str(), nullptr, 10);
    record.detail = fields[9];
    return record;
}

std::uint64_t vmHwmKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            long value = 0;
            status >> value;
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

std::uint64_t since(std::chrono::steady_clock::time_point start)
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return static_cast<std::uint64_t>(elapsed.count() < 0 ? 0 : elapsed.count());
}

Record baseRecord(const std::string &kind, const std::string &name, const std::string &metric, std::uint64_t bytes,
                  std::uint64_t lines, std::uint64_t maxLine)
{
    Record record;
    record.kind = kind;
    record.name = name;
    record.metric = metric;
    record.bytes = bytes;
    record.lines = lines;
    record.maxLine = maxLine;
    record.rssKb = vmHwmKb();
    return record;
}

void inspect(const std::string &bytes, std::uint64_t &lineCount, std::uint64_t &maxLine)
{
    lineCount = 0;
    maxLine = 0;
    std::uint64_t current = 0;
    for (const char c : bytes) {
        ++current;
        if (c == '\n') {
            ++lineCount;
            maxLine = std::max(maxLine, current);
            current = 0;
        }
    }
    if (current > 0 || bytes.empty()) {
        if (current > 0) ++lineCount;
        maxLine = std::max(maxLine, current);
    }
}

std::optional<std::uint64_t> medianOf(std::vector<std::uint64_t> values)
{
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace workload
