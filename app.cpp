#include <fstream>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "src/engine.h"
#ifdef EMJS_BUILD_CONSOLE
#include "src/extension/console.h"
#endif
#ifdef EMJS_BUILD_DATE
#include "src/extension/date.h"
#endif
#ifdef EMJS_BUILD_GLOBAL
#include "src/extension/global.h"
#endif
#ifdef EMJS_BUILD_JSON
#include "src/extension/json.h"
#endif
#ifdef EMJS_BUILD_MATH
#include "src/extension/math.h"
#endif
#ifdef EMJS_BUILD_STRING
#include "src/extension/string.h"
#endif

using namespace Emjs;

namespace {

enum class LineReadResult {
    Ok,
    Eof,
    Interrupted,
};

class TerminalRawMode {
public:
    bool enable()
    {
        if (!isatty(STDIN_FILENO)) return false;
        if (tcgetattr(STDIN_FILENO, &old_) != 0) return false;
        termios raw = old_;
        raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_cflag |= (tcflag_t) CS8;
        raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
        enabled_ = true;
        return true;
    }

    ~TerminalRawMode()
    {
        if (enabled_) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_);
    }

private:
    termios old_ {};
    bool enabled_ = false;
};

void refresh_line(const char* prompt, const std::string& line, std::size_t cursor)
{
    std::cout << '\r' << prompt << line << "\x1b[K";
    std::size_t right = line.size() - cursor;
    if (right > 0) std::cout << "\x1b[" << right << 'D';
    std::cout.flush();
}

LineReadResult read_line_tty(
    const char* prompt,
    std::string& out,
    const std::vector<std::string>& history,
    std::size_t& history_pos,
    std::string& history_draft)
{
    out.clear();
    std::size_t cursor = 0;
    std::cout << prompt << std::flush;

    while (true) {
        char c = 0;
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) return LineReadResult::Eof;

        if (c == '\r' || c == '\n') {
            std::cout << "\r\n";
            return LineReadResult::Ok;
        }
        if (c == 3) {  // Ctrl+C
            std::cout << "\r\n";
            return LineReadResult::Interrupted;
        }
        if (c == 127 || c == 8) {  // Backspace
            if (cursor > 0) {
                out.erase(cursor - 1, 1);
                cursor--;
                refresh_line(prompt, out, cursor);
            }
            continue;
        }
        if (c == 27) {  // Escape sequence (e.g. arrows)
            char seq[2] = {0, 0};
            if (::read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (::read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            if (seq[0] == '[') {
                if (seq[1] == 'D' && cursor > 0) {          // left
                    cursor--;
                    refresh_line(prompt, out, cursor);
                } else if (seq[1] == 'C' && cursor < out.size()) {  // right
                    cursor++;
                    refresh_line(prompt, out, cursor);
                } else if (seq[1] == 'A') {  // up
                    if (history.empty()) continue;
                    if (history_pos == history.size()) history_draft = out;
                    if (history_pos > 0) {
                        history_pos--;
                        out = history[history_pos];
                        cursor = out.size();
                        refresh_line(prompt, out, cursor);
                    }
                } else if (seq[1] == 'B') {  // down
                    if (history.empty()) continue;
                    if (history_pos < history.size()) {
                        history_pos++;
                        if (history_pos == history.size())
                            out = history_draft;
                        else
                            out = history[history_pos];
                        cursor = out.size();
                        refresh_line(prompt, out, cursor);
                    }
                }
            }
            continue;
        }
        if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
            out.insert(cursor, 1, c);
            cursor++;
            refresh_line(prompt, out, cursor);
        }
    }
}

void bind_default_extensions(JsEngine* js)
{
#ifdef EMJS_BUILD_CONSOLE
    EConsole::bind(js);
#endif
#ifdef EMJS_BUILD_STRING
    EString::bind(js);
#endif
#ifdef EMJS_BUILD_GLOBAL
    EGlobal::bind(js);
#endif
#ifdef EMJS_BUILD_DATE
    EDate::bind(js);
#endif
#ifdef EMJS_BUILD_MATH
    EMath::bind(js);
#endif
#ifdef EMJS_BUILD_JSON
    EJson::bind(js);
#endif
}

void print_help(const char* prog)
{
    std::cout << "Emjs CLI " << kVersion << "\n"
              << "Usage:\n"
              << "  " << prog << " <script.js>   Run a script file\n"
              << "  " << prog << " -h|--help     Show help\n"
              << "  " << prog << "               Start interactive mode\n\n"
              << "Interactive mode:\n"
              << "  Enter one line of JavaScript and press Enter to run.\n"
              << "  Press Ctrl+C to exit.\n";
}

bool eval_and_report(JsEngine* js, const char* code, std::size_t len)
{
    JsValue result = js->eval(code, len);
    JsValueType t = JsEngine::getType(result);
    if (t == JsValueType::Error) {
        std::cerr << js->str(result) << std::endl;
        return false;
    }
    if (t != JsValueType::Undefined) {
        std::cout << js->str(result) << std::endl;
    }
    return true;
}

int run_file(JsEngine* js, const char* path)
{
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) {
        std::cerr << "Cannot open file: " << path << std::endl;
        return 1;
    }
    std::string code((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return eval_and_report(js, code.c_str(), code.size()) ? 0 : 1;
}

int run_repl(JsEngine* js)
{
    std::string line;
    std::vector<std::string> history;
    std::size_t history_pos = 0;
    std::string history_draft;
    const bool tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    TerminalRawMode raw;
    const bool raw_ok = tty && raw.enable();
    while (true) {
        if (raw_ok) {
            history_pos = history.size();
            history_draft.clear();
            LineReadResult rr = read_line_tty("> ", line, history, history_pos, history_draft);
            if (rr == LineReadResult::Interrupted || rr == LineReadResult::Eof) break;
        } else {
            if (tty) std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
        }
        if (line.empty()) continue;
        history.push_back(line);
        eval_and_report(js, line.c_str(), line.size());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<char> buffer(600000, 0);
    JsEngine* js = JsEngine::create(buffer.data(), buffer.size());
    if (js == nullptr) {
        std::cerr << "JsEngine::create failed" << std::endl;
        return 2;
    }
    bind_default_extensions(js);

    int rc = 0;
    if (argc >= 2) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            print_help(argv[0]);
        } else {
            rc = run_file(js, argv[1]);
        }
    } else {
        rc = run_repl(js);
    }

    JsEngine::destroy(js);
    return rc;
}

