#pragma once
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace ytfast {
inline int run_process(const std::vector<std::string>& args, bool quiet = false) {
    if (args.empty()) return 127;
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) return 127;
    if (child == 0) {
        if (quiet) {
            int sink = ::open("/dev/null", O_WRONLY);
            if (sink >= 0) { ::dup2(sink, STDOUT_FILENO); ::dup2(sink, STDERR_FILENO); ::close(sink); }
        }
        ::execvp(argv[0], argv.data()); ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
// All transcode inputs and partial outputs are owned by this invocation.
// The final hard link is exclusive, including when another process races us.
struct OutputStage {
    std::string directory, output, final;
    explicit OutputStage(const std::string& destination, const std::string& fallback) : final(destination) {
        struct stat state{};
        if (::lstat(final.c_str(), &state) == 0) throw std::runtime_error("output already exists: " + final);
        auto pattern = std::filesystem::absolute(final).string() + ".work.XXXXXX";
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create private output staging directory");
        directory = pattern;
        auto extension = std::filesystem::path(final).extension().string();
        output = directory + "/output" + (extension.empty() ? fallback : extension);
    }
    OutputStage(const OutputStage&) = delete;
    OutputStage& operator=(const OutputStage&) = delete;
    ~OutputStage() {
        for (const auto& name : {output, directory + "/video.src", directory + "/audio.src"}) {
            ::unlink(name.c_str()); ::unlink((name + ".part").c_str()); ::unlink((name + ".part.ranges").c_str());
        }
        ::rmdir(directory.c_str());
    }
    bool finalize() const { return ::link(output.c_str(), final.c_str()) == 0; }
};
inline bool convert_audio(const std::string& input, const std::string& destination) {
    try {
        OutputStage stage(destination, ".mp3");
        if (run_process({"ffmpeg", "-n", "-loglevel", "error", "-i", input,
                         "-vn", "-map", "0:a:0", stage.output}, true) != 0) return false;
        return stage.finalize();
    } catch (const std::exception&) { return false; }
}
} // namespace ytfast
