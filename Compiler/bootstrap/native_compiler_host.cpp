#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#error "Noqeri native self-host CLI currently targets Linux."
#endif

extern "C" std::int64_t stage2CompileNativeBuffer(
    std::uint8_t* source,
    std::size_t source_len,
    int target,
    std::uint8_t* output,
    std::size_t output_len);

extern "C" std::int64_t stage2NativeCheckBuffer(
    std::uint8_t* source,
    std::size_t source_len,
    int target);

namespace {

std::string trim(std::string value) {
    auto first = std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); });
    auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool redundantForwardPrototype(const std::string& clean) {
    if (clean.rfind("function ", 0) != 0) return false;
    if (clean.find('{') != std::string::npos) return false;
    return clean.find('(') != std::string::npos && clean.find(')') != std::string::npos;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data, std::size_t count) {
    if (count > data.size()) throw std::runtime_error("native compiler returned an invalid output size");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(count));
    if (!output) throw std::runtime_error("failed writing " + path.string());
}

std::vector<std::uint8_t> bundleManifest(const std::string& rootPath, const std::string& manifestPath) {
    namespace fs = std::filesystem;
    const fs::path root = fs::weakly_canonical(fs::path(rootPath));
    std::ifstream manifest(manifestPath);
    if (!manifest) throw std::runtime_error("cannot open source manifest " + manifestPath);

    std::ostringstream merged;
    merged << "module noqeri.compiler.selfhost.bundle\n";
    std::unordered_set<std::string> seen;
    std::string relative;
    std::size_t sourceCount = 0;

    while (std::getline(manifest, relative)) {
        relative = trim(relative);
        if (relative.empty() || relative[0] == '#') continue;
        const fs::path listed(relative);
        if (listed.is_absolute() || listed.has_root_name() || listed.has_root_directory())
            throw std::runtime_error("source manifest requires repository-relative paths: " + relative);
        if (listed.extension() != ".noqeri")
            throw std::runtime_error("source manifest accepts only .noqeri files: " + relative);

        const fs::path candidate = fs::weakly_canonical(root / listed);
        const auto rel = candidate.lexically_relative(root);
        if (rel.empty() || *rel.begin() == ".." || rel == ".")
            throw std::runtime_error("source manifest escapes repository root: " + relative);
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec)
            throw std::runtime_error("source manifest entry is not a regular file: " + relative);
        if (!seen.insert(candidate.generic_string()).second)
            throw std::runtime_error("duplicate source in manifest: " + relative);

        ++sourceCount;
        std::ifstream input(candidate, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open compiler source " + candidate.string());
        std::string line;
        while (std::getline(input, line)) {
            const auto clean = trim(line);
            if (clean.rfind("module ", 0) == 0 || clean.rfind("import ", 0) == 0) continue;
            if (redundantForwardPrototype(clean)) continue;
            merged << line << '\n';
        }
        merged << '\n';
    }

    if (manifest.bad()) throw std::runtime_error("failed reading source manifest " + manifestPath);
    if (sourceCount == 0) throw std::runtime_error("source manifest contains no .noqeri files: " + manifestPath);
    const auto text = merged.str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::size_t compileAssembly(std::vector<std::uint8_t>& source, int target, std::vector<std::uint8_t>& output) {
    const auto written = stage2CompileNativeBuffer(source.data(), source.size(), target, output.data(), output.size());
    if (written < 0) throw std::runtime_error("native Noqeri compiler failed with status " + std::to_string(written));
    const auto count = static_cast<std::uint64_t>(written);
    if (count > output.size()) throw std::runtime_error("native compiler output exceeds host buffer");
    return static_cast<std::size_t>(count);
}

int runCxx(const std::string& assembly, const std::string& output) {
    const auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        execlp("c++", "c++", "-O2", "-x", "assembler", assembly.c_str(), "-o", output.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) throw std::runtime_error("waitpid failed");
    if (!WIFEXITED(status)) return 128;
    return WEXITSTATUS(status);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr
                << "usage:\n"
                << "  noqeri-native check <source.noqeri> [target]\n"
                << "  noqeri-native emit-asm <source.noqeri> <output.s> [target]\n"
                << "  noqeri-native compile-manifest <repo-root> <manifest> <output.s> [target]\n"
                << "  noqeri-native build <source.noqeri> <output> [target]\n";
            return 2;
        }

        const std::string command = argv[1];
        if (command == "check") {
            if (argc < 3) throw std::runtime_error("check requires a source path");
            auto source = readFile(argv[2]);
            const int target = argc >= 4 ? std::stoi(argv[3]) : 3;
            const auto status = stage2NativeCheckBuffer(source.data(), source.size(), target);
            if (status != 0) {
                std::cerr << argv[2] << ": native compiler check failed (" << status << ")\n";
                return 1;
            }
            std::cout << argv[2] << ": native compiler check OK\n";
            return 0;
        }

        constexpr std::size_t kOutputCapacity = 128ull * 1024ull * 1024ull;
        std::vector<std::uint8_t> output(kOutputCapacity);

        if (command == "emit-asm") {
            if (argc < 4) throw std::runtime_error("emit-asm requires source and output paths");
            auto source = readFile(argv[2]);
            const int target = argc >= 5 ? std::stoi(argv[4]) : 3;
            const auto written = compileAssembly(source, target, output);
            writeFile(argv[3], output, written);
            std::cout << "Wrote native Noqeri assembly: " << argv[3] << " (" << written << " bytes)\n";
            return 0;
        }

        if (command == "compile-manifest") {
            if (argc < 5) throw std::runtime_error("compile-manifest requires root, manifest and output paths");
            auto source = bundleManifest(argv[2], argv[3]);
            const int target = argc >= 6 ? std::stoi(argv[5]) : 3;
            const auto written = compileAssembly(source, target, output);
            writeFile(argv[4], output, written);
            std::cout << "Wrote native self-host assembly: " << argv[4] << " (" << written << " bytes)\n";
            return 0;
        }

        if (command == "build") {
            if (argc < 4) throw std::runtime_error("build requires source and output paths");
            auto source = readFile(argv[2]);
            const int target = argc >= 5 ? std::stoi(argv[4]) : 3;
            const auto written = compileAssembly(source, target, output);
            const std::string assembly = std::string(argv[3]) + ".noqeri-native.s";
            writeFile(assembly, output, written);
            const int status = runCxx(assembly, argv[3]);
            std::error_code ec;
            std::filesystem::remove(assembly, ec);
            if (status != 0) {
                std::cerr << "system assembler/linker failed with status " << status << "\n";
                return status;
            }
            std::cout << "Built native executable: " << argv[3] << "\n";
            return 0;
        }

        throw std::runtime_error("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "noqeri-native: " << error.what() << "\n";
        return 1;
    }
}
