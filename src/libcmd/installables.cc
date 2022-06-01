#include "globals.hh"
#include "installables.hh"
#include "util.hh"
#include "command.hh"
#include "attr-path.hh"
#include "common-eval-args.hh"
#include "derivations.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "get-drvs.hh"
#include "store-api.hh"
#include "shared.hh"
#include "flake/flake.hh"
#include "eval-cache.hh"
#include "url.hh"
#include "registry.hh"
#include "build-result.hh"

#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

const static std::regex attrPathRegex(
    R"((?:[a-zA-Z0-9_"-][a-zA-Z0-9_".,^\*-]*))",
    std::regex::ECMAScript);

void completeFlakeInputPath(
    ref<EvalState> evalState,
    const FlakeRef & flakeRef,
    std::string_view prefix)
{
    auto flake = flake::getFlake(*evalState, flakeRef, true);
    for (auto & input : flake.inputs)
        if (hasPrefix(input.first, prefix))
            completions->add(input.first);
}

MixFlakeOptions::MixFlakeOptions()
{
    auto category = "Common flake-related options";

    addFlag({
        .longName = "recreate-lock-file",
        .description = "Recreate the flake's lock file from scratch.",
        .category = category,
        .handler = {&lockFlags.recreateLockFile, true}
    });

    addFlag({
        .longName = "no-update-lock-file",
        .description = "Do not allow any updates to the flake's lock file.",
        .category = category,
        .handler = {&lockFlags.updateLockFile, false}
    });

    addFlag({
        .longName = "no-write-lock-file",
        .description = "Do not write the flake's newly generated lock file.",
        .category = category,
        .handler = {&lockFlags.writeLockFile, false}
    });

    addFlag({
        .longName = "no-registries",
        .description =
          "Don't allow lookups in the flake registries. This option is deprecated; use `--no-use-registries`.",
        .category = category,
        .handler = {[&]() {
            lockFlags.useRegistries = false;
            warn("'--no-registries' is deprecated; use '--no-use-registries'");
        }}
    });

    addFlag({
        .longName = "commit-lock-file",
        .description = "Commit changes to the flake's lock file.",
        .category = category,
        .handler = {&lockFlags.commitLockFile, true}
    });

    addFlag({
        .longName = "update-input",
        .description = "Update a specific flake input (ignoring its previous entry in the lock file).",
        .category = category,
        .labels = {"input-path"},
        .handler = {[&](std::string s) {
            lockFlags.inputUpdates.insert(flake::parseInputPath(s));
        }},
        .completer = {[&](size_t, std::string_view prefix) {
            if (auto flakeRef = getFlakeRefForCompletion())
                completeFlakeInputPath(getEvalState(), *flakeRef, prefix);
        }}
    });

    addFlag({
        .longName = "override-input",
        .description = "Override a specific flake input (e.g. `dwarffs/nixpkgs`). This implies `--no-write-lock-file`.",
        .category = category,
        .labels = {"input-path", "flake-url"},
        .handler = {[&](std::string inputPath, std::string flakeRef) {
            lockFlags.writeLockFile = false;
            lockFlags.inputOverrides.insert_or_assign(
                flake::parseInputPath(inputPath),
                parseFlakeRef(flakeRef, absPath("."), true));
        }},
        .completer = {[&](size_t n, std::string_view prefix) {
            if (n == 0) {
                if (auto flakeRef = getFlakeRefForCompletion())
                    completeFlakeInputPath(getEvalState(), *flakeRef, prefix);
            } else if (n == 1) {
                completeFlakeRef(getEvalState()->store, prefix);
            }
        }}
    });

    addFlag({
        .longName = "inputs-from",
        .description = "Use the inputs of the specified flake as registry entries.",
        .category = category,
        .labels = {"flake-url"},
        .handler = {[&](std::string flakeRef) {
            auto evalState = getEvalState();
            auto flake = flake::lockFlake(
                *evalState,
                parseFlakeRef(flakeRef, absPath(".")),
                { .writeLockFile = false });
            for (auto & [inputName, input] : flake.lockFile.root->inputs) {
                auto input2 = flake.lockFile.findInput({inputName}); // resolve 'follows' nodes
                if (auto input3 = std::dynamic_pointer_cast<const flake::LockedNode>(input2)) {
                    overrideRegistry(
                        fetchers::Input::fromAttrs({{"type","indirect"}, {"id", inputName}}),
                        input3->lockedRef.input,
                        {});
                }
            }
        }},
        .completer = {[&](size_t, std::string_view prefix) {
            completeFlakeRef(getEvalState()->store, prefix);
        }}
    });
}

SourceExprCommand::SourceExprCommand(bool supportReadOnlyMode)
{
    addFlag({
        .longName = "file",
        .shortName = 'f',
        .description =
            "Interpret installables as attribute paths relative to the Nix expression stored in *file*. "
            "If *file* is the character -, then a Nix expression will be read from standard input.",
        .category = installablesCategory,
        .labels = {"file"},
        .handler = {&file},
        .completer = completePath
    });

    addFlag({
        .longName = "expr",
        .description = "Interpret installables as attribute paths relative to the Nix expression *expr*.",
        .category = installablesCategory,
        .labels = {"expr"},
        .handler = {&expr}
    });

    addFlag({
        .longName = "derivation",
        .description = "Operate on the store derivation rather than its outputs.",
        .category = installablesCategory,
        .handler = {&operateOn, OperateOn::Derivation},
    });

    addFlag({
        .longName = "apply-to-installable",
        .description = "Apply the function *expr* to each installable.",
        .category = installablesCategory,
        .labels = {"expr"},
        .handler = {&applyToInstallable},
    });

    addFlag({
        .longName = "override",
        .description = "Override derivation arguments: --override *name* *expr*",
        .category = installablesCategory,
        .labels = {"name", "expr"},
        .handler = {[&](std::string name, std::string expr) {  overrideArgs[name] = 'E' + expr; }}
    });

    addFlag({
        .longName = "override-pkg",
        .description = "Override dependency *name* of the given derivations with other packages, using *flakeref*.",
        .category = installablesCategory,
        .labels = {"name", "flakeref"},
        .handler = {[&](std::string name, std::string expr) {  overrideArgs[name] = 'F' + expr; }}
    });

    addFlag({
        .longName = "override-from",
        .description = "Override dependency *name* of the given derivations with other packages, using *attrpath*. (Only applicable when using --file)",
        .category = installablesCategory,
        .labels = {"name", "attrpath"},
        .handler = {[&](std::string name, std::string expr) {  overrideArgs[name] = 'X' + expr; }}
    });

    addFlag({
        .longName = "override-attrs",
        .description = "Override the given derivations' attributes using *expr*.",
        .category = installablesCategory,
        .labels = {"expr"},
        .handler = {&installableOverrideAttrs},
    });

    addFlag({
        .longName = "with",
        .description = "Enhance the given derivations using an environment specified in *expr*.",
        .category = installablesCategory,
        .labels = {"expr"},
        .handler = {&installableWithPackages},
    });

    if (supportReadOnlyMode) {
        addFlag({
            .longName = "read-only",
            .description =
                "Do not instantiate each evaluated derivation. "
                "This improves performance, but can cause errors when accessing "
                "store paths of derivations during evaluation.",
            .handler = {&readOnlyMode, true},
        });
    }
}

Strings SourceExprCommand::getDefaultFlakeAttrPaths()
{
    return {
        "packages." + settings.thisSystem.get() + ".default",
        "defaultPackage." + settings.thisSystem.get()
    };
}

Strings SourceExprCommand::getDefaultFlakeAttrPathPrefixes()
{
    return {
        // As a convenience, look for the attribute in
        // 'outputs.packages'.
        "packages." + settings.thisSystem.get() + ".",
        // As a temporary hack until Nixpkgs is properly converted
        // to provide a clean 'packages' set, look in 'legacyPackages'.
        "legacyPackages." + settings.thisSystem.get() + "."
    };
}

void SourceExprCommand::completeInstallable(std::string_view prefix)
{
    if (file) {
        completionType = ctAttrs;

        evalSettings.pureEval = false;
        auto state = getEvalState();
        Expr *e = state->parseExprFromFile(
            resolveExprPath(state->checkSourcePath(lookupFileArg(*state, *file)))
        );

        Value root;
        state->eval(e, root);

        auto autoArgs = getAutoArgs(*state);

        std::string prefix_ = std::string(prefix);
        auto sep = prefix_.rfind('.');
        std::string searchWord;
        if (sep != std::string::npos) {
            searchWord = prefix_.substr(sep + 1, std::string::npos);
            prefix_ = prefix_.substr(0, sep);
        } else {
            searchWord = prefix_;
            prefix_ = "";
        }

        auto [v, pos] = findAlongAttrPath(*state, prefix_, *autoArgs, root);
        Value &v1(*v);
        state->forceValue(v1, pos);
        Value v2;
        state->autoCallFunction(*autoArgs, v1, v2);

        if (v2.type() == nAttrs) {
            for (auto & i : *v2.attrs) {
                std::string name = state->symbols[i.name];
                if (name.find(searchWord) == 0) {
                    if (prefix_ == "")
                        completions->add(name);
                    else
                        completions->add(prefix_ + "." + name);
                }
            }
        }
    } else {
        completeFlakeRefWithFragment(
            getEvalState(),
            lockFlags,
            getDefaultFlakeAttrPathPrefixes(),
            getDefaultFlakeAttrPaths(),
            prefix);
    }
}

void completeFlakeRefWithFragment(
    ref<EvalState> evalState,
    flake::LockFlags lockFlags,
    Strings attrPathPrefixes,
    const Strings & defaultFlakeAttrPaths,
    std::string_view prefix)
{
    /* Look for flake output attributes that match the
       prefix. */
    try {
        bool isAttrPath = std::regex_match(prefix.begin(), prefix.end(), attrPathRegex);
        auto hash = prefix.find('#');
        if (!isAttrPath && hash == std::string::npos) {
            completeFlakeRef(evalState->store, prefix);
        } else {

            completionType = ctAttrs;

            auto fragment =
                isAttrPath
                ? prefix
                : prefix.substr(hash + 1);

            auto flakeRefS =
                isAttrPath
                ? std::string("flake:default")
                : std::string(prefix.substr(0, hash));

            auto flakeRef = parseFlakeRef(expandTilde(flakeRefS), absPath("."));

            auto evalCache = openEvalCache(*evalState,
                std::make_shared<flake::LockedFlake>(lockFlake(*evalState, flakeRef, lockFlags)));

            auto root = evalCache->getRoot();

            /* Complete 'fragment' relative to all the
               attrpath prefixes as well as the root of the
               flake. */
            attrPathPrefixes.push_back("");

            for (auto & attrPathPrefixS : attrPathPrefixes) {
                auto attrPathPrefix = parseAttrPath(*evalState, attrPathPrefixS);
                auto attrPathS = attrPathPrefixS + std::string(fragment);
                auto attrPath = parseAttrPath(*evalState, attrPathS);

                std::string lastAttr;
                if (!attrPath.empty() && !hasSuffix(attrPathS, ".")) {
                    lastAttr = evalState->symbols[attrPath.back()];
                    attrPath.pop_back();
                }

                auto attr = root->findAlongAttrPath(attrPath);
                if (!attr) continue;

                for (auto & attr2 : (*attr)->getAttrs()) {
                    if (hasPrefix(evalState->symbols[attr2], lastAttr)) {
                        auto attrPath2 = (*attr)->getAttrPath(attr2);
                        /* Strip the attrpath prefix. */
                        attrPath2.erase(attrPath2.begin(), attrPath2.begin() + attrPathPrefix.size());
                        if (isAttrPath)
                            completions->add(concatStringsSep(".", evalState->symbols.resolve(attrPath2)));
                        else
                            completions->add(flakeRefS + "#" + concatStringsSep(".", evalState->symbols.resolve(attrPath2)));
                    }
                }
            }

            /* And add an empty completion for the default
               attrpaths. */
            if (fragment.empty()) {
                for (auto & attrPath : defaultFlakeAttrPaths) {
                    auto attr = root->findAlongAttrPath(parseAttrPath(*evalState, attrPath));
                    if (!attr) continue;
                    completions->add(flakeRefS + "#");
                }
            }
        }
    } catch (Error & e) {
        warn(e.msg());
    }
}

void completeFlakeRef(ref<Store> store, std::string_view prefix)
{
    if (!settings.isExperimentalFeatureEnabled(Xp::Flakes))
        return;

    if (prefix == "")
        completions->add(".");

    completeDir(0, prefix);

    /* Look for registry entries that match the prefix. */
    for (auto & registry : fetchers::getRegistries(store)) {
        for (auto & entry : registry->entries) {
            auto from = entry.from.to_string();
            if (!hasPrefix(prefix, "flake:") && hasPrefix(from, "flake:")) {
                std::string from2(from, 6);
                if (hasPrefix(from2, prefix))
                    completions->add(from2);
            } else {
                if (hasPrefix(from, prefix))
                    completions->add(from);
            }
        }
    }
}

DerivedPath Installable::toDerivedPath()
{
    auto buildables = toDerivedPaths();
    if (buildables.size() != 1)
        throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(), buildables.size());
    return std::move(buildables[0]);
}

std::vector<ref<eval_cache::AttrCursor>>
Installable::getCursors(EvalState & state)
{
    auto evalCache =
        std::make_shared<nix::eval_cache::EvalCache>(std::nullopt, state,
            [&]() { return toValue(state).first; });
    return {evalCache->getRoot()};
}

ref<eval_cache::AttrCursor>
Installable::getCursor(EvalState & state)
{
    auto cursors = getCursors(state);
    if (cursors.empty())
        throw Error("cannot find flake attribute '%s'", what());
    return cursors[0];
}

static StorePath getDeriver(
    ref<Store> store,
    const Installable & i,
    const StorePath & drvPath)
{
    auto derivers = store->queryValidDerivers(drvPath);
    if (derivers.empty())
        throw Error("'%s' does not have a known deriver", i.what());
    // FIXME: use all derivers?
    return *derivers.begin();
}

struct InstallableStorePath : Installable
{
    ref<Store> store;
    StorePath storePath;

    InstallableStorePath(ref<Store> store, StorePath && storePath)
        : store(store), storePath(std::move(storePath)) { }

    std::string what() const override { return store->printStorePath(storePath); }

    DerivedPaths toDerivedPaths() override
    {
        if (storePath.isDerivation()) {
            auto drv = store->readDerivation(storePath);
            return {
                DerivedPath::Built {
                    .drvPath = storePath,
                    .outputs = drv.outputNames(),
                }
            };
        } else {
            return {
                DerivedPath::Opaque {
                    .path = storePath,
                }
            };
        }
    }

    StorePathSet toDrvPaths(ref<Store> store) override
    {
        if (storePath.isDerivation()) {
            return {storePath};
        } else {
            return {getDeriver(store, *this, storePath)};
        }
    }

    std::optional<StorePath> getStorePath() override
    {
        return storePath;
    }
};

DerivedPaths InstallableValue::toDerivedPaths()
{
    DerivedPaths res;

    std::map<StorePath, std::set<std::string>> drvsToOutputs;
    RealisedPath::Set drvsToCopy;

    // Group by derivation, helps with .all in particular
    for (auto & drv : toDerivations()) {
        for (auto & outputName : drv.outputsToInstall)
            drvsToOutputs[drv.drvPath].insert(outputName);
        drvsToCopy.insert(drv.drvPath);
    }

    for (auto & i : drvsToOutputs)
        res.push_back(DerivedPath::Built { i.first, i.second });

    return res;
}

StorePathSet InstallableValue::toDrvPaths(ref<Store> store)
{
    StorePathSet res;
    for (auto & drv : toDerivations())
        res.insert(drv.drvPath);
    return res;
}

struct InstallableAttrPath : InstallableValue
{
    SourceExprCommand & cmd;
    RootValue v;
    std::string attrPath;
    OutputsSpec outputsSpec;

    InstallableAttrPath(
        ref<EvalState> state,
        SourceExprCommand & cmd,
        Value * v,
        const std::string & attrPath,
        OutputsSpec outputsSpec)
        : InstallableValue(state)
        , cmd(cmd)
        , v(allocRootValue(v))
        , attrPath(attrPath)
        , outputsSpec(std::move(outputsSpec))
    { }

    std::string what() const override { return attrPath; }

    std::pair<Value *, PosIdx> toValue(EvalState & state) override
    {
        auto [vRes, pos] = findAlongAttrPath(state, attrPath, *cmd.getAutoArgs(state), **v);
        state.forceValue(*vRes, pos);
        return {vRes, pos};
    }

    virtual std::vector<InstallableValue::DerivationInfo> toDerivations() override;
};

std::vector<InstallableValue::DerivationInfo> InstallableAttrPath::toDerivations()
{
    auto v = toValue(*state).first;

    Bindings & autoArgs = *cmd.getAutoArgs(*state);

    DrvInfos drvInfos;
    getDerivations(*state, *v, "", autoArgs, drvInfos, false);

    std::vector<InstallableValue::DerivationInfo> res;
    for (auto & drvInfo : drvInfos) {
        auto drvPath = drvInfo.queryDrvPath();
        if (!drvPath)
            throw Error("'%s' is not a derivation", what());

        std::set<std::string> outputsToInstall;

        if (auto outputNames = std::get_if<OutputNames>(&outputsSpec))
            outputsToInstall = *outputNames;
        else
            for (auto & output : drvInfo.queryOutputs(false, std::get_if<DefaultOutputs>(&outputsSpec)))
                outputsToInstall.insert(output.first);

        res.push_back(DerivationInfo {
            .drvPath = *drvPath,
            .outputsToInstall = std::move(outputsToInstall)
        });
    }

    return res;
}

std::vector<std::string> InstallableFlake::getActualAttrPaths()
{
    std::vector<std::string> res;

    for (auto & prefix : prefixes)
        res.push_back(prefix + *attrPaths.begin());

    for (auto & s : attrPaths)
        res.push_back(s);

    return res;
}

Value * InstallableFlake::getFlakeOutputs(EvalState & state, const flake::LockedFlake & lockedFlake)
{
    auto vFlake = state.allocValue();

    callFlake(state, lockedFlake, *vFlake);

    auto aOutputs = vFlake->attrs->get(state.symbols.create("outputs"));
    assert(aOutputs);

    state.forceValue(*aOutputs->value, [&]() { return aOutputs->value->determinePos(noPos); });

    return aOutputs->value;
}

ref<eval_cache::EvalCache> openEvalCache(
    EvalState & state,
    std::shared_ptr<flake::LockedFlake> lockedFlake)
{
    auto fingerprint = lockedFlake->getFingerprint();
    return make_ref<nix::eval_cache::EvalCache>(
        evalSettings.useEvalCache && evalSettings.pureEval
            ? std::optional { std::cref(fingerprint) }
            : std::nullopt,
        state,
        [&state, lockedFlake]()
        {
            /* For testing whether the evaluation cache is
               complete. */
            if (getEnv("NIX_ALLOW_EVAL").value_or("1") == "0")
                throw Error("not everything is cached, but evaluation is not allowed");

            auto vFlake = state.allocValue();
            flake::callFlake(state, *lockedFlake, *vFlake);

            state.forceAttrs(*vFlake, noPos);

            auto aOutputs = vFlake->attrs->get(state.symbols.create("outputs"));
            assert(aOutputs);

            return aOutputs->value;
        });
}

static std::string showAttrPaths(const std::vector<std::string> & paths)
{
    std::string s;
    for (const auto & [n, i] : enumerate(paths)) {
        if (n > 0) s += n + 1 == paths.size() ? " or " : ", ";
        s += '\''; s += i; s += '\'';
    }
    return s;
}

InstallableFlake::InstallableFlake(
    SourceExprCommand * cmd,
    ref<EvalState> state,
    FlakeRef && flakeRef,
    std::string_view fragment,
    OutputsSpec outputsSpec,
    Strings attrPaths,
    Strings prefixes,
    const flake::LockFlags & lockFlags)
    : InstallableValue(state),
      flakeRef(flakeRef),
      attrPaths(fragment == "" ? attrPaths : Strings{(std::string) fragment}),
      prefixes(fragment == "" ? Strings{} : prefixes),
      outputsSpec(std::move(outputsSpec)),
      lockFlags(lockFlags)
{
    if (cmd && cmd->getAutoArgs(*state)->size())
        throw UsageError("'--arg' and '--argstr' are incompatible with flakes");
}

std::tuple<std::string, FlakeRef, InstallableValue::DerivationInfo> InstallableFlake::toDerivation()
{
    auto attr = getCursor(*state);

    auto attrPath = attr->getAttrPathStr();

    if (!attr->isDerivation())
        throw Error("flake output attribute '%s' is not a derivation", attrPath);

    auto drvPath = attr->forceDerivation();

    std::set<std::string> outputsToInstall;
    std::optional<NixInt> priority;

    if (auto aOutputSpecified = attr->maybeGetAttr(state->sOutputSpecified)) {
        if (aOutputSpecified->getBool()) {
            if (auto aOutputName = attr->maybeGetAttr("outputName"))
                outputsToInstall = { aOutputName->getString() };
        }
    }

    else if (auto aMeta = attr->maybeGetAttr(state->sMeta)) {
        if (auto aOutputsToInstall = aMeta->maybeGetAttr("outputsToInstall"))
            for (auto & s : aOutputsToInstall->getListOfStrings())
                outputsToInstall.insert(s);
        if (auto aPriority = aMeta->maybeGetAttr("priority"))
            priority = aPriority->getInt();
    }

    if (outputsToInstall.empty() || std::get_if<AllOutputs>(&outputsSpec)) {
        outputsToInstall.clear();
        if (auto aOutputs = attr->maybeGetAttr(state->sOutputs))
            for (auto & s : aOutputs->getListOfStrings())
                outputsToInstall.insert(s);
    }

    if (outputsToInstall.empty())
        outputsToInstall.insert("out");

    if (auto outputNames = std::get_if<OutputNames>(&outputsSpec))
        outputsToInstall = *outputNames;

    auto drvInfo = DerivationInfo {
        .drvPath = std::move(drvPath),
        .outputsToInstall = std::move(outputsToInstall),
        .priority = priority,
    };

    return {attrPath, getLockedFlake()->flake.lockedRef, std::move(drvInfo)};
}

std::vector<InstallableValue::DerivationInfo> InstallableFlake::toDerivations()
{
    std::vector<InstallableValue::DerivationInfo> res;
    res.push_back(std::get<2>(toDerivation()));
    return res;
}

std::pair<Value *, PosIdx> InstallableFlake::toValue(EvalState & state)
{
    return {&getCursor(state)->forceValue(), noPos};
}

std::vector<ref<eval_cache::AttrCursor>>
InstallableFlake::getCursors(EvalState & state)
{
    auto evalCache = openEvalCache(state,
        std::make_shared<flake::LockedFlake>(lockFlake(state, flakeRef, lockFlags)));

    auto root = evalCache->getRoot();

    std::vector<ref<eval_cache::AttrCursor>> res;

    for (auto & attrPath : getActualAttrPaths()) {
        auto attr = root->findAlongAttrPath(parseAttrPath(state, attrPath));
        if (attr) res.push_back(ref(*attr));
    }

    return res;
}

ref<eval_cache::AttrCursor> InstallableFlake::getCursor(EvalState & state)
{
    auto lockedFlake = getLockedFlake();

    auto cache = openEvalCache(state, lockedFlake);
    auto root = cache->getRoot();

    Suggestions suggestions;

    auto attrPaths = getActualAttrPaths();

    for (auto & attrPath : attrPaths) {
        debug("trying flake output attribute '%s'", attrPath);

        auto attrOrSuggestions = root->findAlongAttrPath(
            parseAttrPath(state, attrPath),
            true
        );

        if (!attrOrSuggestions) {
            suggestions += attrOrSuggestions.getSuggestions();
            continue;
        }

        return *attrOrSuggestions;
    }

    throw Error(
        suggestions,
        "flake '%s' does not provide attribute %s",
        flakeRef,
        showAttrPaths(attrPaths));
}

std::shared_ptr<flake::LockedFlake> InstallableFlake::getLockedFlake() const
{
    if (!_lockedFlake) {
        flake::LockFlags lockFlagsApplyConfig = lockFlags;
        lockFlagsApplyConfig.applyNixConfig = true;
        _lockedFlake = std::make_shared<flake::LockedFlake>(lockFlake(*state, flakeRef, lockFlagsApplyConfig));
    }
    return _lockedFlake;
}

FlakeRef InstallableFlake::nixpkgsFlakeRef() const
{
    auto lockedFlake = getLockedFlake();

    if (auto nixpkgsInput = lockedFlake->lockFile.findInput({"nixpkgs"})) {
        if (auto lockedNode = std::dynamic_pointer_cast<const flake::LockedNode>(nixpkgsInput)) {
            debug("using nixpkgs flake '%s'", lockedNode->lockedRef);
            return std::move(lockedNode->lockedRef);
        }
    }

    return Installable::nixpkgsFlakeRef();
}

Bindings * SourceExprCommand::getOverrideArgs(EvalState & state, ref<Store> store)
{
    Bindings * res = state.allocBindings(overrideArgs.size());
    for (auto & i : overrideArgs) {
        Value * v = state.allocValue();
        if (i.second[0] == 'E') {
            // is a raw expression, parse
            state.mkThunk_(*v, state.parseExprFromString(std::string(i.second, 1), absPath(".")));
        } else if (i.second[0] == 'F') {
            // is a flakeref
            auto [vNew, pos] = parseInstallable(store, std::string(i.second, 1), false, false)->toValue(state);
            v = vNew;
        } else if (i.second[0] == 'X') {
            // is not a flakeref, use attrPath from file
            // error because --override covers expressions already
            if (!file)
                throw Error("no file to reference override from");
            auto [vNew, pos] = parseInstallable(store, std::string(i.second, 1), false, true)->toValue(state);
            v = vNew;
        } else {
            throw Error("[BUG] unknown argtype %s",i.second[0]);
        }
        res->push_back(Attr(state.symbols.create(i.first), v));
    }
    res->sort();
    return res;
}

std::vector<std::shared_ptr<Installable>> SourceExprCommand::parseInstallables(
    ref<Store> store, std::vector<std::string> ss,
    bool applyOverrides, bool nestedIsExprOk)
{
    std::vector<std::shared_ptr<Installable>> result;

    if (readOnlyMode) {
        settings.readOnlyMode = true;
    }

    auto modifyInstallable = applyOverrides && ( applyToInstallable
        || installableOverrideAttrs || installableWithPackages || overrideArgs.size() > 0 );

    if (nestedIsExprOk && (file || expr)) {
        if (file && expr)
            throw UsageError("'--file' and '--expr' are exclusive");

        // FIXME: backward compatibility hack
        if (file) evalSettings.pureEval = false;

        auto state = getEvalState();
        auto vFile = state->allocValue();

        if (file == "-") {
            auto e = state->parseStdin();
            state->eval(e, *vFile);
        } else if (file)
            state->evalFile(lookupFileArg(*state, *file), *vFile);
        else {
            auto e = state->parseExprFromString(*expr, absPath("."));
            state->eval(e, *vFile);
        }

        for (auto & s : ss) {
            auto [prefix, outputsSpec] = parseOutputsSpec(s);
            auto installableAttr = std::make_shared<InstallableAttrPath>(state, *this, vFile, prefix == "." ? "" : prefix, outputsSpec);
            if (modifyInstallable) {
                auto [v, pos] = installableAttr->toValue(*state);
                auto vApply = state->allocValue();
                auto vRes = state->allocValue();
                auto state = getEvalState();
                auto overrideSet = getOverrideArgs(*state, store);

                // FIXME merge this and the code for the flake version into a single function ("modifyInstallables()"?)
                if (applyToInstallable) {
                    state->eval(state->parseExprFromString(*applyToInstallable, absPath(".")), *vApply);
                    state->callFunction(*vApply, *v, *vRes, noPos);
                } else if (overrideSet->size() > 0) {
                    Value * overrideValues = state->allocValue();
                    overrideValues->mkAttrs(state->buildBindings(overrideSet->size()).finish());
                    for (auto& v : *overrideSet) {
                        overrideValues->attrs->push_back(v);
                    }
                    auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("override"));
                    if (!vOverrideFunctorAttr) {
                        throw Error("%s is not overridable", s);
                    }
                    auto vOverrideFunctor = vOverrideFunctorAttr->value;
                    state->callFunction(*vOverrideFunctor, *overrideValues, *vRes, noPos);
                } else if (installableOverrideAttrs) {
                    state->eval(state->parseExprFromString(fmt("old: with old; %s",*installableOverrideAttrs), absPath(".")), *vApply);
                    auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("overrideAttrs"));
                    if (!vOverrideFunctorAttr) {
                        throw Error("%s is not overrideAttrs-capable", s);
                    }
                    auto vOverrideFunctor = vOverrideFunctorAttr->value;
                    state->callFunction(*vOverrideFunctor, *vApply, *vRes, noPos);
                } else if (installableWithPackages) {
                    state->eval(state->parseExprFromString(fmt("ps: with ps; %s",*installableWithPackages), absPath(".")), *vApply);
                    auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("withPackages"));
                    if (!vOverrideFunctorAttr) {
                        throw Error("%s cannot be extended with additional packages", s);
                    }
                    auto vOverrideFunctor = vOverrideFunctorAttr->value;
                    state->callFunction(*vOverrideFunctor, *vApply, *vRes, noPos);
                }
                result.push_back(std::make_shared<InstallableAttrPath>(state, *this, vRes, prefix == "." ? "" : prefix, outputsSpec));
            } else {
                result.push_back(installableAttr);
            }
        }
    } else {

        for (auto & s : ss) {
            std::exception_ptr ex;

            if (s.find('/') != std::string::npos) {
                /*if (modifyInstallable) {
                    throw Error("cannot apply function: installable cannot be evaluated");
                }*/
                try {
                    result.push_back(std::make_shared<InstallableStorePath>(store, store->followLinksToStorePath(s)));
                    continue;
                } catch (BadStorePath &) {
                } catch (...) {
                    if (!ex)
                        ex = std::current_exception();
                }
            }

            try {
                bool isAttrPath = std::regex_match(s, attrPathRegex);
                
                auto actualRef = isAttrPath ? "flake:default#" + s : s;

                auto [flakeRef, fragment, outputsSpec] = parseFlakeRefWithFragmentAndOutputsSpec(actualRef, absPath("."));

                auto state = getEvalState();
                auto installableFlake = std::make_shared<InstallableFlake>(
                        this,
                        state,
                        std::move(flakeRef),
                        fragment,
                        outputsSpec,
                        getDefaultFlakeAttrPaths(),
                        getDefaultFlakeAttrPathPrefixes(),
                        lockFlags);
                if (modifyInstallable) {
                    auto [v, pos] = installableFlake->toValue(*state);
                    auto vApply = state->allocValue();
                    auto vRes = state->allocValue();
                    auto state = getEvalState();
                    auto overrideSet = getOverrideArgs(*state, store);

                    if (applyToInstallable) {
                        state->eval(state->parseExprFromString(*applyToInstallable, absPath(".")), *vApply);
                        state->callFunction(*vApply, *v, *vRes, noPos);
                    } else if (overrideSet->size() > 0) {
                        Value * overrideValues = state->allocValue();
                        overrideValues->mkAttrs(state->buildBindings(overrideSet->size()).finish());
                        for (auto& v : *overrideSet) {
                            overrideValues->attrs->push_back(v);
                        }
                        auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("override"));
                        if (!vOverrideFunctorAttr) {
                            throw Error("%s is not overridable", s);
                        }
                        auto vOverrideFunctor = vOverrideFunctorAttr->value;
                        state->callFunction(*vOverrideFunctor, *overrideValues, *vRes, noPos);
                    } else if (installableOverrideAttrs) {
                        state->eval(state->parseExprFromString(fmt("old: with old; %s",*installableOverrideAttrs), absPath(".")), *vApply);
                        auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("overrideAttrs"));
                        if (!vOverrideFunctorAttr) {
                            throw Error("%s is not overrideAttrs-capable", s);
                        }
                        auto vOverrideFunctor = vOverrideFunctorAttr->value;
                        state->callFunction(*vOverrideFunctor, *vApply, *vRes, noPos);
                    } else if (installableWithPackages) {
                        state->eval(state->parseExprFromString(fmt("ps: with ps; %s",*installableWithPackages), absPath(".")), *vApply);
                        auto vOverrideFunctorAttr = v->attrs->get(state->symbols.create("withPackages"));
                        if (!vOverrideFunctorAttr) {
                            throw Error("%s cannot be extended with additional packages", s);
                        }
                        auto vOverrideFunctor = vOverrideFunctorAttr->value;
                        state->callFunction(*vOverrideFunctor, *vApply, *vRes, noPos);
                    }
                    result.push_back(std::make_shared<InstallableAttrPath>(state, *this, vRes, "", outputsSpec));
                } else {
                    result.push_back(installableFlake);
                }
                continue;
            } catch (...) {
                ex = std::current_exception();
            }

            std::rethrow_exception(ex);
        }
    }

    return result;
}

std::shared_ptr<Installable> SourceExprCommand::parseInstallable(
    ref<Store> store, const std::string & installable,
    bool applyOverrides, bool nestedIsExprOk)
{
    auto installables = parseInstallables(store, {installable}, applyOverrides, nestedIsExprOk);
    assert(installables.size() == 1);
    return installables.front();
}

BuiltPaths Installable::build(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode,
    const std::vector<std::shared_ptr<Installable>> & installables,
    BuildMode bMode)
{
    BuiltPaths res;
    for (auto & [_, builtPath] : build2(evalStore, store, mode, installables, bMode))
        res.push_back(builtPath);
    return res;
}

std::vector<std::pair<std::shared_ptr<Installable>, BuiltPath>> Installable::build2(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode,
    const std::vector<std::shared_ptr<Installable>> & installables,
    BuildMode bMode)
{
    if (mode == Realise::Nothing)
        settings.readOnlyMode = true;

    std::vector<DerivedPath> pathsToBuild;
    std::map<DerivedPath, std::vector<std::shared_ptr<Installable>>> backmap;

    for (auto & i : installables) {
        for (auto b : i->toDerivedPaths()) {
            pathsToBuild.push_back(b);
            backmap[b].push_back(i);
        }
    }

    std::vector<std::pair<std::shared_ptr<Installable>, BuiltPath>> res;

    switch (mode) {

    case Realise::Nothing:
    case Realise::Derivation:
        printMissing(store, pathsToBuild, lvlError);

        for (auto & path : pathsToBuild) {
            for (auto & installable : backmap[path]) {
                std::visit(overloaded {
                    [&](const DerivedPath::Built & bfd) {
                        OutputPathMap outputs;
                        auto drv = evalStore->readDerivation(bfd.drvPath);
                        auto outputHashes = staticOutputHashes(*evalStore, drv); // FIXME: expensive
                        auto drvOutputs = drv.outputsAndOptPaths(*store);
                        for (auto & output : bfd.outputs) {
                            auto outputHash = get(outputHashes, output);
                            if (!outputHash)
                                throw Error(
                                    "the derivation '%s' doesn't have an output named '%s'",
                                    store->printStorePath(bfd.drvPath), output);
                            if (settings.isExperimentalFeatureEnabled(Xp::CaDerivations)) {
                                DrvOutput outputId { *outputHash, output };
                                auto realisation = store->queryRealisation(outputId);
                                if (!realisation)
                                    throw Error(
                                        "cannot operate on an output of the "
                                        "unbuilt derivation '%s'",
                                        outputId.to_string());
                                outputs.insert_or_assign(output, realisation->outPath);
                            } else {
                                // If ca-derivations isn't enabled, assume that
                                // the output path is statically known.
                                auto drvOutput = get(drvOutputs, output);
                                assert(drvOutput);
                                assert(drvOutput->second);
                                outputs.insert_or_assign(
                                    output, *drvOutput->second);
                            }
                        }
                        res.push_back({installable, BuiltPath::Built { bfd.drvPath, outputs }});
                    },
                    [&](const DerivedPath::Opaque & bo) {
                        res.push_back({installable, BuiltPath::Opaque { bo.path }});
                    },
                }, path.raw());
            }
        }

        break;

    case Realise::Outputs: {
        for (auto & buildResult : store->buildPathsWithResults(pathsToBuild, bMode, evalStore)) {
            if (!buildResult.success())
                buildResult.rethrow();

            for (auto & installable : backmap[buildResult.path]) {
                std::visit(overloaded {
                    [&](const DerivedPath::Built & bfd) {
                        std::map<std::string, StorePath> outputs;
                        for (auto & path : buildResult.builtOutputs)
                            outputs.emplace(path.first.outputName, path.second.outPath);
                        res.push_back({installable, BuiltPath::Built { bfd.drvPath, outputs }});
                    },
                    [&](const DerivedPath::Opaque & bo) {
                        res.push_back({installable, BuiltPath::Opaque { bo.path }});
                    },
                }, buildResult.path.raw());
            }
        }

        break;
    }

    default:
        assert(false);
    }

    return res;
}

BuiltPaths Installable::toBuiltPaths(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode,
    OperateOn operateOn,
    const std::vector<std::shared_ptr<Installable>> & installables)
{
    if (operateOn == OperateOn::Output)
        return Installable::build(evalStore, store, mode, installables);
    else {
        if (mode == Realise::Nothing)
            settings.readOnlyMode = true;

        BuiltPaths res;
        for (auto & drvPath : Installable::toDerivations(store, installables, true))
            res.push_back(BuiltPath::Opaque{drvPath});
        return res;
    }
}

StorePathSet Installable::toStorePaths(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode, OperateOn operateOn,
    const std::vector<std::shared_ptr<Installable>> & installables)
{
    StorePathSet outPaths;
    for (auto & path : toBuiltPaths(evalStore, store, mode, operateOn, installables)) {
        auto thisOutPaths = path.outPaths();
        outPaths.insert(thisOutPaths.begin(), thisOutPaths.end());
    }
    return outPaths;
}

StorePath Installable::toStorePath(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode, OperateOn operateOn,
    std::shared_ptr<Installable> installable)
{
    auto paths = toStorePaths(evalStore, store, mode, operateOn, {installable});

    if (paths.size() != 1)
        throw Error("argument '%s' should evaluate to one store path", installable->what());

    return *paths.begin();
}

StorePathSet Installable::toDerivations(
    ref<Store> store,
    const std::vector<std::shared_ptr<Installable>> & installables,
    bool useDeriver)
{
    StorePathSet drvPaths;

    for (const auto & i : installables)
        for (const auto & b : i->toDerivedPaths())
            std::visit(overloaded {
                [&](const DerivedPath::Opaque & bo) {
                    if (!useDeriver)
                        throw Error("argument '%s' did not evaluate to a derivation", i->what());
                    drvPaths.insert(getDeriver(store, *i, bo.path));
                },
                [&](const DerivedPath::Built & bfd) {
                    drvPaths.insert(bfd.drvPath);
                },
            }, b.raw());

    return drvPaths;
}

InstallablesCommand::InstallablesCommand()
{
    expectArgs({
        .label = "installables",
        .handler = {&_installables},
        .completer = {[&](size_t, std::string_view prefix) {
            completeInstallable(prefix);
        }}
    });
}

void InstallablesCommand::prepare()
{
    if (_installables.empty() && useDefaultInstallables())
        // FIXME: commands like "nix profile install" should not have a
        // default, probably.
        _installables.push_back(".");
    installables = parseInstallables(getStore(), _installables);
}

std::optional<FlakeRef> InstallablesCommand::getFlakeRefForCompletion()
{
    if (_installables.empty()) {
        if (useDefaultInstallables())
            return parseFlakeRefWithFragment(".", absPath(".")).first;
        return {};
    }
    return parseFlakeRefWithFragment(_installables.front(), absPath(".")).first;
}

InstallableCommand::InstallableCommand(bool supportReadOnlyMode)
    : SourceExprCommand(supportReadOnlyMode)
{
    expectArgs({
        .label = "installable",
        .optional = true,
        .handler = {&_installable},
        .completer = {[&](size_t, std::string_view prefix) {
            completeInstallable(prefix);
        }}
    });
}

void InstallableCommand::prepare()
{
    installable = parseInstallable(getStore(), _installable);
}

}
