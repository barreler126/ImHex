 #pragma once

#include <hex/api/achievement_manager.hpp>
#include <hex/ui/view.hpp>
#include <hex/ui/popup.hpp>
#include <hex/providers/provider.hpp>
#include <hex/helpers/default_paths.hpp>

#include <pl/pattern_language.hpp>
#include <pl/core/errors/error.hpp>
#include <pl/core/lexer.hpp>

#include <ui/hex_editor.hpp>
#include <ui/pattern_drawer.hpp>
#include <ui/visualizer_drawer.hpp>

#include <filesystem>
#include <functional>

#include <TextEditor.h>
#include <popups/popup_file_chooser.hpp>
#include <content/text_highlighting/pattern_language.hpp>

namespace pl::ptrn { class Pattern; }

namespace hex::plugin::builtin {


    constexpr static auto textEditorView    = "/##pattern_editor_";
    constexpr static auto consoleView       = "/##console_";
    constexpr static auto variablesView     = "/##env_vars_";
    constexpr static auto settingsView      = "/##settings_";
    constexpr static auto virtualFilesView  = "/##Virtual_File_Tree_";
    constexpr static auto debuggerView      = "/##debugger_";

    class PatternSourceCode {
    public:
        const std::string& get(prv::Provider *provider) const {
            if (m_synced)
                return m_sharedSource;

            return m_perProviderSource.get(provider);
        }

        std::string& get(prv::Provider *provider) {
            if (m_synced)
                return m_sharedSource;

            return m_perProviderSource.get(provider);
        }

        bool isSynced() const {
            return m_synced;
        }

        void enableSync(bool enabled) {
            m_synced = enabled;
        }

    private:
        bool m_synced = false;
        PerProvider<std::string> m_perProviderSource;
        std::string m_sharedSource;
    };

    class ViewPatternEditor : public View::Window {
    public:
        ViewPatternEditor();
        ~ViewPatternEditor() override;

        void drawAlwaysVisibleContent() override;
        std::unique_ptr<pl::PatternLanguage> *getPatternLanguage() {
            return &m_editorRuntime;
        }

        TextEditor &getTextEditor() {
            return m_textEditor;
        }

        bool getChangesWereParsed() const {
            return m_changesWereParsed;
        }

        u32  getRunningParsers () const {
            return m_runningParsers;
        }

        u32  getRunningEvaluators () const {
            return m_runningEvaluators;
        }

        void setChangesWereParsed(bool changesWereParsed) {
            m_changesWereParsed = changesWereParsed;
        }

        void drawContent() override;
        [[nodiscard]] ImGuiWindowFlags getWindowFlags() const override {
            return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        }

        void setPopupWindowHeight(u32 height) { m_popupWindowHeight = height; }
        u32 getPopupWindowHeight() const { return m_popupWindowHeight; }

        struct VirtualFile {
            std::fs::path path;
            std::vector<u8> data;
            Region region;
        };

        enum class DangerousFunctionPerms : u8 {
            Ask,
            Allow,
            Deny
        };

    private:
        class PopupAcceptPattern : public Popup<PopupAcceptPattern> {
        public:
            explicit PopupAcceptPattern(ViewPatternEditor *view) : Popup("hex.builtin.view.pattern_editor.accept_pattern"), m_view(view) {}

            void drawContent() override {
                std::scoped_lock lock(m_view->m_possiblePatternFilesMutex);

                auto* provider = ImHexApi::Provider::get();

                ImGuiExt::TextFormattedWrapped("{}", static_cast<const char *>("hex.builtin.view.pattern_editor.accept_pattern.desc"_lang));

                if (ImGui::BeginListBox("##patterns_accept", ImVec2(400_scaled, 0))) {
                    u32 index = 0;
                    for (const auto &[path, author, description] : m_view->m_possiblePatternFiles.get(provider)) {
                        ImGui::PushID(index + 1);
                        auto fileName = wolv::util::toUTF8String(path.filename());

                        std::string displayValue;
                        if (!description.empty()) {
                            displayValue = fmt::format("{} ({})", description, fileName);
                        } else {
                            displayValue = fileName;
                        }

                        if (ImGui::Selectable(displayValue.c_str(), index == m_selectedPatternFile, ImGuiSelectableFlags_NoAutoClosePopups))
                            m_selectedPatternFile = index;

                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal)) {
                            if (ImGui::BeginTooltip()) {
                                ImGui::TextUnformatted(fileName.c_str());

                                if (!author.empty()) {
                                    ImGui::SameLine();
                                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                                    ImGui::SameLine();
                                    ImGui::TextUnformatted(author.c_str());
                                }

                                if (!description.empty()) {
                                    ImGui::Separator();
                                    ImGui::TextUnformatted(description.c_str());
                                }

                                ImGui::EndTooltip();
                            }
                        }

                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                            m_view->loadPatternFile(m_view->m_possiblePatternFiles.get(provider)[m_selectedPatternFile].path, provider, false);

                        ImGuiExt::InfoTooltip(wolv::util::toUTF8String(path).c_str());

                        index++;

                        ImGui::PopID();
                    }

                    // Close the popup if there aren't any patterns available
                    if (index == 0)
                        this->close();

                    ImGui::EndListBox();
                }

                ImGui::NewLine();
                ImGui::TextUnformatted("hex.builtin.view.pattern_editor.accept_pattern.question"_lang);
                ImGui::NewLine();

                ImGuiExt::ConfirmButtons("hex.ui.common.yes"_lang, "hex.ui.common.no"_lang,
                    [this, provider] {
                        m_view->loadPatternFile(m_view->m_possiblePatternFiles.get(provider)[m_selectedPatternFile].path, provider, false);
                        this->close();
                    },
                    [this] {
                        this->close();
                    }
                );

                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    this->close();
            }

            [[nodiscard]] ImGuiWindowFlags getFlags() const override {
                return ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize;
            }

        private:
            ViewPatternEditor *m_view;
            u32 m_selectedPatternFile = 0;
        };

    private:
        struct PatternVariable {
            bool inVariable;
            bool outVariable;

            pl::core::Token::ValueType type;
            pl::core::Token::Literal value;
        };

        enum class EnvVarType
        {
            Integer,
            Float,
            String,
            Bool
        };

        struct EnvVar {
            u64 id;
            std::string name;
            pl::core::Token::Literal value;
            EnvVarType type;

            bool operator==(const EnvVar &other) const {
                return this->id == other.id;
            }
        };

        struct AccessData {
            float progress;
            u32 color;
        };

        struct PossiblePattern {
            std::fs::path path;
            std::string author;
            std::string description;
        };

        std::unique_ptr<pl::PatternLanguage> m_editorRuntime;

        std::mutex m_possiblePatternFilesMutex;
        PerProvider<std::vector<PossiblePattern>> m_possiblePatternFiles;
        bool m_runAutomatically   = false;
        bool m_triggerEvaluation  = false;
        std::atomic<bool> m_triggerAutoEvaluate = false;

        volatile bool m_lastEvaluationProcessed = true;
        bool m_lastEvaluationResult    = false;

        std::atomic<u32> m_runningEvaluators = 0;
        std::atomic<u32> m_runningParsers    = 0;

        bool m_changesWereParsed = false;
        PerProvider<bool> m_hasUnevaluatedChanges;
        std::chrono::time_point<std::chrono::steady_clock> m_lastEditorChangeTime;

        PerProvider<TextEditor> m_textEditor, m_consoleEditor;
        std::atomic<bool> m_consoleNeedsUpdate = false;

        std::atomic<bool> m_dangerousFunctionCalled = false;
        std::atomic<DangerousFunctionPerms> m_dangerousFunctionsAllowed = DangerousFunctionPerms::Ask;

        bool m_autoLoadPatterns = true;

        PerProvider<ui::VisualizerDrawer> m_visualizerDrawer;
        bool m_tooltipJustOpened = false;

        PatternSourceCode m_sourceCode;
        PerProvider<std::vector<std::string>> m_console;
        PerProvider<bool> m_executionDone;

        std::mutex m_logMutex;

        PerProvider<TextEditor::Coordinates>  m_cursorPosition;

        PerProvider<TextEditor::Coordinates> m_consoleCursorPosition;
        PerProvider<bool> m_cursorNeedsUpdate;
        PerProvider<bool> m_consoleCursorNeedsUpdate;
        PerProvider<TextEditor::Selection> m_selection;
        PerProvider<TextEditor::Selection> m_consoleSelection;
        PerProvider<size_t> m_consoleLongestLineLength;
        PerProvider<TextEditor::Breakpoints> m_breakpoints;
        PerProvider<std::optional<pl::core::err::PatternLanguageError>> m_lastEvaluationError;
        PerProvider<std::vector<pl::core::err::CompileError>> m_lastCompileError;
        PerProvider<const std::vector<pl::core::Evaluator::StackTrace>*> m_callStack;
        PerProvider<std::map<std::string, pl::core::Token::Literal>> m_lastEvaluationOutVars;
        PerProvider<std::map<std::string, PatternVariable>> m_patternVariables;

        PerProvider<std::vector<VirtualFile>> m_virtualFiles;

        PerProvider<std::list<EnvVar>> m_envVarEntries;

        PerProvider<TaskHolder> m_analysisTask;
        PerProvider<bool> m_shouldAnalyze;
        PerProvider<bool> m_breakpointHit;
        PerProvider<std::unique_ptr<ui::PatternDrawer>> m_debuggerDrawer;
        std::atomic<bool> m_resetDebuggerVariables;
        int m_debuggerScopeIndex = 0;

        std::array<AccessData, 512> m_accessHistory = {};
        u32 m_accessHistoryIndex = 0;
        bool m_parentHighlightingEnabled = true;
        bool m_replaceMode = false;
        bool m_openFindReplacePopUp = false;
        bool m_openGotoLinePopUp = false;
        bool m_patternEvaluating = false;
        std::map<std::fs::path, std::string> m_patternNames;
        PerProvider<wolv::io::ChangeTracker> m_changeTracker;
        PerProvider<bool> m_ignoreNextChangeEvent;
        PerProvider<bool> m_changeEventAcknowledgementPending;
        PerProvider<bool> m_patternFileDirty;

        ImRect m_textEditorHoverBox;
        ImRect m_consoleHoverBox;
        std::string m_focusedSubWindowName;
        float m_popupWindowHeight = 0;
        float m_popupWindowHeightChange = 0;
        bool m_frPopupIsClosed = true;
        bool m_gotoPopupIsClosed = true;

        static inline std::array<std::string,256> m_findHistory;
        static inline u32 m_findHistorySize = 0;
        static inline u32 m_findHistoryIndex = 0;
        static inline std::array<std::string,256> m_replaceHistory;
        static inline u32 m_replaceHistorySize = 0;
        static inline u32 m_replaceHistoryIndex = 0;

        TextHighlighter m_textHighlighter = TextHighlighter(this,&this->m_editorRuntime);
    private:
        void drawConsole(ImVec2 size);
        void drawEnvVars(ImVec2 size, std::list<EnvVar> &envVars);
        void drawVariableSettings(ImVec2 size, std::map<std::string, PatternVariable> &patternVariables);
        void drawVirtualFiles(ImVec2 size, const std::vector<VirtualFile> &virtualFiles) const;
        void drawDebugger(ImVec2 size);

        void drawPatternTooltip(pl::ptrn::Pattern *pattern);

        void drawTextEditorFindReplacePopup(TextEditor *textEditor);
        void drawTextEditorGotoLinePopup(TextEditor *textEditor);

        void historyInsert(std::array<std::string, 256> &history, u32 &size, u32 &index, const std::string &value);

        void loadPatternFile(const std::fs::path &path, prv::Provider *provider, bool trackFile = false);
        bool isPatternDirty(prv::Provider *provider) { return m_patternFileDirty.get(provider); }
        void markPatternFileDirty(prv::Provider *provider) { m_patternFileDirty.get(provider) = true; }

        void parsePattern(const std::string &code, prv::Provider *provider);
        void evaluatePattern(const std::string &code, prv::Provider *provider);

        TextEditor *getEditorFromFocusedWindow();
        void setupFindReplace(TextEditor *editor);
        void setupGotoLine(TextEditor *editor);

        void registerEvents();
        void registerMenuItems();
        void registerHandlers();

        void handleFileChange(prv::Provider *provider);

        std::function<void(bool)> m_openPatternFile = [this](bool trackFile) {
            auto provider = ImHexApi::Provider::get();
            if (provider == nullptr)
                return;
            const auto basePaths = paths::Patterns.read();
            std::vector<std::fs::path> paths;

            for (const auto &imhexPath : basePaths) {
                if (!wolv::io::fs::exists(imhexPath)) continue;

                std::error_code error;
                for (auto &entry : std::fs::recursive_directory_iterator(imhexPath, error)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".hexpat")
                        paths.push_back(entry.path());
                }
            }

            auto createRuntime = [provider] {
                auto runtime = std::make_shared<pl::PatternLanguage>();
                ContentRegistry::PatternLanguage::configureRuntime(*runtime, provider);

                return runtime;
            };

            ui::PopupNamedFileChooser::open(
                basePaths, paths, std::vector<hex::fs::ItemFilter>{ { "Pattern File", "hexpat" } }, false,
                [this, runtime = createRuntime()](const std::fs::path &path, const std::fs::path &adjustedPath) mutable -> std::string {
                    if (auto it = m_patternNames.find(path); it != m_patternNames.end()) {
                        return it->second;
                    }

                    const auto fileName = wolv::util::toUTF8String(adjustedPath.filename());
                    m_patternNames[path] = fileName;

                    wolv::io::File file(path, wolv::io::File::Mode::Read);
                    pl::api::Source source(file.readString());

                    // Only run the lexer on the source file and manually extract the #pragma description to make this
                    // process as fast as possible. Running the preprocessor directly takes too much time
                    auto result = runtime->getInternals().lexer->lex(&source);
                    if (result.isOk()) {
                        const auto tokens = result.unwrap();
                        for (auto it = tokens.begin(); it != tokens.end(); ++it) {
                            if (it->type == pl::core::Token::Type::Directive && std::get<pl::core::Token::Directive>(it->value) == pl::core::Token::Directive::Pragma) {
                                ++it;
                                if (it != tokens.end() && it->type == pl::core::Token::Type::String) {
                                    auto literal = std::get<pl::core::Token::Literal>(it->value);
                                    auto string = std::get_if<std::string>(&literal);
                                    if (string != nullptr && *string == "description") {
                                        ++it;
                                        if (it != tokens.end() && it->type == pl::core::Token::Type::String) {
                                            literal = std::get<pl::core::Token::Literal>(it->value);
                                            string = std::get_if<std::string>(&literal);
                                            if (string != nullptr) {
                                                m_patternNames[path] = hex::format("{} ({})", *string, fileName);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    return m_patternNames[path];
                },
                [this, provider, trackFile](const std::fs::path &path) {
                    this->loadPatternFile(path, provider, trackFile);
                    AchievementManager::unlockAchievement("hex.builtin.achievement.patterns", "hex.builtin.achievement.patterns.load_existing.name");
                }
            );
        };

        std::function<void(bool)> m_savePatternFile = [this](bool trackFile) {
            auto provider = ImHexApi::Provider::get();
            if (provider == nullptr)
                return;
            auto path = m_changeTracker.get(provider).getPath();
            wolv::io::File file(path, wolv::io::File::Mode::Write);
            if (file.isValid() && trackFile) {
                if (isPatternDirty(provider)) {
                    file.writeString(wolv::util::trim(m_textEditor.get(provider).GetText()));
                    m_patternFileDirty.get(provider) = false;
                }
                return;
            }
            m_savePatternAsFile(trackFile);
        };

        std::function<void(bool)> m_savePatternAsFile = [this](bool trackFile) {
            auto provider = ImHexApi::Provider::get();
            if (provider == nullptr)
                return;
            fs::openFileBrowser(
                    fs::DialogMode::Save, { {"Pattern File", "hexpat"}, {"Pattern Import File", "pat"} },
                    [this, provider, trackFile](const auto &path) {
                        wolv::io::File file(path, wolv::io::File::Mode::Create);
                        file.writeString(wolv::util::trim(m_textEditor.get(provider).GetText()));
                        m_patternFileDirty.get(provider) = false;
                        auto loadedPath = m_changeTracker.get(provider).getPath();
                        if ((loadedPath.empty() && loadedPath != path) || (!loadedPath.empty() && !trackFile))
                            m_changeTracker.get(provider).stopTracking();

                        if (trackFile) {
                            m_changeTracker.get(provider) = wolv::io::ChangeTracker(file);
                            m_changeTracker.get(provider).startTracking([this, provider]{ this->handleFileChange(provider); });
                            m_ignoreNextChangeEvent.get(provider) = true;
                        }
                    }
            );
        };

        void appendEditorText(const std::string &text);
        void appendVariable(const std::string &type);
        void appendArray(const std::string &type, size_t size);
    };

}
