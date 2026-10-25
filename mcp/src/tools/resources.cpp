/**
 * @file resources.cpp
 * @brief 内置资源：bacio:///quote（随机名言）与 document:///life-choices.txt。
 *
 * @details
 * 参考项目把资源定义在插件里，`resources/read` 走插件的 `HandleRequest`，
 * 文本从编译期目录硬编码读取。这里资源就是两个值 + 读取函数，文本位置
 * 由 `MCP_RESOURCE_DIR` 环境变量覆盖，默认相对当前工作目录的 `resources/`。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace go2cpp::mcp::tools {

namespace {

/// 名言库：意大利语爱情/接吻主题（与参考资源同名同主题）。
const std::vector<std::string>& Quotes() {
    static const std::vector<std::string> quotes = {
        "E che cos'è un bacio? Un apostrofo rosa fra le parole t'amo. (Edmond Rostand)",
        "Amore non è guardarsi a vicenda; è guardare insieme nella stessa direzione. "
        "(Antoine de Saint-Exupery)",
        "Amare è mettere la nostra felicità nella felicità di un altro. (Gottfried Leibniz)",
        "I baci sono le monete spicciole dell'amore. (Taddeus)",
        "Amor che nella mente mi ragiona cominciò egli a dir si dolcemente. (Dante Alighieri)",
        "A chi più amiamo, meno dire sappiamo. (Proverbio inglese)",
        "Amare e fai quel che vuoi. (Sant'Agostino)",
        "Amore e tosse non si possono nascondere. (Ovidio)",
        "Amor s'io posso uscir dei tuoi artigli appena creder posso che alcun altro uncin mai mi pigli. "
        "(Giovanni Boccaccio)",
        "Amore guarda non con gli occhi ma con l'anima. (William Shakespeare)",
        "Bacio: abbandono del cuore quando non è più solo. (Anonimo)",
        "Dall'amicizia all'amore c'è la distanza di un bacio. (Anonimo)",
        "Dice più un bacio che una dichiarazione d'amore. (Anonimo)",
        "Fa del tuo amore una pioggia di baci sulle mie labbra. (Percy Bysshe Shelley)",
        "Fra due cuori che si amano non occorrono parole. (Marceline Desbordes-Valmore)",
        "I baci mettono a nudo il cuore e vestono l'amore. (Anonimo)",
        "L'amore è una cosa meravigliosa. (Anonimo)",
        "Non c'è niente di più dolce di un bacio dato con amore. (Anonimo)",
        "Il bacio è un segreto detto sulla bocca. (Edmond Rostand)",
        "Ogni bacio ha un suo sapore. (Anonimo)",
        "Per un bacio, l'anima vola. (Anonimo)",
        "Un bacio è un attimo di infinito. (Anonimo)",
        "L'amore vince sempre. (Virgilio)",
        "Bacia la bocca che ti ama. (Anonimo)",
    };
    return quotes;
}

/// 取资源目录：环境变量优先；否则探测 "resources" 与 "mcp/resources"，
/// 让「从项目根运行」和「从 mcp/ 运行」都能找到同一份资源。
std::string ResourceDirectory() {
    const char* from_env = std::getenv("MCP_RESOURCE_DIR");
    if (from_env != nullptr && *from_env != '\0') {
        return from_env;
    }
    namespace fs = std::filesystem;
    std::error_code code;
    for (const char* candidate : {"resources", "mcp/resources"}) {
        if (fs::is_directory(candidate, code)) {
            return candidate;
        }
    }
    return "resources";
}

/// 读取文本文件；失败时返回空串并把错误写进 error。
bool ReadTextFile(const std::string& path, std::string& content, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "无法打开资源文件: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        error = "读取资源文件失败: " + path;
        return false;
    }
    content = buffer.str();
    return true;
}

json MakeContents(const std::string& uri, const std::string& mime, std::string text) {
    json block = json::object();
    block["uri"] = uri;
    block["mimeType"] = mime;
    block["text"] = std::move(text);
    json result = json::object();
    result["contents"] = json::array({block});
    return result;
}

}  // namespace

void RegisterBuiltinResources(Registry& registry) {
    // bacio:///quote
    {
        Resource resource;
        resource.uri = "bacio:///quote";
        resource.name = "bacio-quote";
        resource.description = "随机返回一条关于爱与亲吻的名言";
        resource.mime_type = "text/plain";
        resource.reader = [](const json&) {
            static std::mt19937 generator(static_cast<unsigned>(std::time(nullptr)));
            const auto& quotes = Quotes();
            std::uniform_int_distribution<std::size_t> distribution(0, quotes.size() - 1);
            return MakeContents("bacio:///quote", "text/plain", quotes[distribution(generator)]);
        };
        registry.RegisterResource(std::move(resource));
    }

    // document:///life-choices.txt
    {
        Resource resource;
        resource.uri = "document:///life-choices.txt";
        resource.name = "life-choices";
        resource.description = "短篇故事 life-choices.txt";
        resource.mime_type = "text/plain";
        resource.reader = [](const json&) {
            std::string content;
            std::string error;
            const std::string path = ResourceDirectory() + "/life_choices.txt";
            if (!ReadTextFile(path, content, error)) {
                throw std::runtime_error(error);
            }
            return MakeContents("document:///life-choices.txt", "text/plain", std::move(content));
        };
        registry.RegisterResource(std::move(resource));
    }
}

}  // namespace go2cpp::mcp::tools
