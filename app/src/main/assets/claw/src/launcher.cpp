#include "launcher.hpp"

#include <cstdlib>
#include <sstream>

#include "json.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace claw {

using json = nlohmann::json;

namespace {

bool is_executable_file(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
#ifdef _WIN32
  return true;
#else
  return access(p.c_str(), X_OK) == 0;
#endif
}

bool is_script(const fs::path& p) {
  auto ext = to_lower(p.extension().string());
  return ext == ".mjs" || ext == ".js" || ext == ".cjs";
}

}  // namespace

std::string find_in_path(const std::string& exe) {
  std::string path = get_env("PATH");
#ifdef _WIN32
  const char sep = ';';
  std::vector<std::string> exts = {".exe", ".cmd", ".bat", ""};
#else
  const char sep = ':';
  std::vector<std::string> exts = {""};
#endif
  std::stringstream ss(path);
  std::string dir;
  while (std::getline(ss, dir, sep)) {
    if (dir.empty()) continue;
    for (const auto& ext : exts) {
      fs::path cand = fs::path(dir) / (exe + ext);
      if (is_executable_file(cand)) return cand.string();
    }
  }
  return "";
}

OpenClawCommand find_openclaw() {
  OpenClawCommand cmd;
  auto use = [&](const fs::path& p, const std::string& source) {
    cmd.found = true;
    cmd.source = source;
    if (is_script(p)) {
      std::string node = find_in_path("node");
      cmd.prefix = {node.empty() ? "node" : node, p.string()};
    } else {
      cmd.prefix = {p.string()};
    }
  };

  std::string env = get_env("OPENCLAW_BIN");
  if (!env.empty()) {
    use(fs::path(env), "OPENCLAW_BIN");
    return cmd;
  }

  // Instalasi resmi (installer OpenClaw memasang wrapper di ~/.openclaw/bin).
  fs::path home_bin = home_dir() / ".openclaw" / "bin";
#ifdef _WIN32
  for (const char* n : {"openclaw.cmd", "openclaw.exe", "openclaw.ps1"}) {
    if (is_executable_file(home_bin / n) && std::string(n).find(".ps1") == std::string::npos) {
      use(home_bin / n, "~/.openclaw/bin");
      return cmd;
    }
  }
#else
  if (is_executable_file(home_bin / "openclaw")) {
    use(home_bin / "openclaw", "~/.openclaw/bin");
    return cmd;
  }
#endif

  std::string on_path = find_in_path("openclaw");
  if (!on_path.empty()) {
    use(fs::path(on_path), "PATH");
    return cmd;
  }

  return cmd;
}

std::vector<fs::path> project_skill_dirs(const fs::path& project_root, const fs::path& workspace) {
  std::vector<fs::path> dirs;
  std::error_code weq;
  bool in_workspace = !workspace.empty() && fs::equivalent(workspace, project_root, weq);
  for (const auto& rel : {fs::path("skills"), fs::path(".agents") / "skills", fs::path("claude-skills")}) {
    std::error_code ec;
    if (in_workspace && rel != "claude-skills") continue;
    fs::path d = project_root / rel;
    if (fs::is_directory(d, ec)) dirs.push_back(fs::absolute(d));
  }
  return dirs;
}

namespace {
void count_skills_in(const fs::path& dir, int depth, int& n) {
  std::error_code ec;
  if (fs::exists(dir / "SKILL.md", ec)) {
    ++n;
    return;
  }
  if (depth >= 2) return;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    std::string name = it->path().filename().string();
    if (it->is_directory(ec2) && name != "node_modules" && name[0] != '.') count_skills_in(it->path(), depth + 1, n);
  }
}
}  // namespace

int count_skills(const fs::path& root) {
  int n = 0;
  count_skills_in(root, 0, n);
  return n;
}

void write_openclaw_config(const fs::path& config_path, int port, const std::string& token,
                           const fs::path& workspace, const std::vector<fs::path>& skill_dirs) {
  json model = {{"id", "auto"},
                {"name", "Claw Pool (auto-switch)"},
                {"reasoning", false},
                {"input", json::array({"text"})},
                {"contextWindow", 131072},
                {"maxTokens", 8192}};
  json cfg = {
      {"models",
       {{"mode", "merge"},
        {"providers",
         {{"clawpool",
           {{"baseUrl", "http://127.0.0.1:" + std::to_string(port) + "/v1"},
            {"apiKey", token},
            {"api", "openai-completions"},
            {"models", json::array({model})}}}}}}},
      {"agents",
       {{"defaults",
         {{"model", {{"primary", "clawpool/auto"}}},
          {"workspace", workspace.generic_string()}}}}}};
  if (!skill_dirs.empty()) {
    json dirs = json::array();
    for (const auto& d : skill_dirs) dirs.push_back(d.generic_string());
    cfg["skills"] = {{"load", {{"extraDirs", dirs}}}};
  }
  write_file_atomic(config_path, cfg.dump(2) + "\n");
}

int run_command(const std::vector<std::string>& argv) {
  std::string line;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) line += " ";
    line += shell_quote(argv[i]);
  }
#ifdef _WIN32
  // cmd.exe membuang pasangan kutip terluar; bungkus sekali lagi.
  line = "\"" + line + "\"";
#endif
  int rc = std::system(line.c_str());
#ifdef _WIN32
  return rc;
#else
  if (rc == -1) return 127;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
  return rc;
#endif
}

}  // namespace claw
