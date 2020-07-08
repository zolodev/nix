#include "derivations.hh"
#include "store-api.hh"
#include "globals.hh"
#include "util.hh"
#include "worker-protocol.hh"
#include "fs-accessor.hh"
#include "istringstream_nocopy.hh"

namespace nix {

// FIXME Put this somewhere?
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

StorePath DerivationOutput::path(const Store & store, string drvName) const
{
    return std::visit(overloaded {
        [](DerivationOutputIntensional doi) {
            return doi.path;
        },
        [&](DerivationOutputFixed dof) {
            return store.makeFixedOutputPath(dof.hash.method, dof.hash.hash, drvName);
        }
    }, output);
}

const StorePath & BasicDerivation::findOutput(const Store & store, const string & id) const
{
    auto i = outputs.find(id);
    if (i == outputs.end())
        throw Error("derivation has no output '%s'", id);
    return i->second.path(store, name);
}


bool BasicDerivation::isBuiltin() const
{
    return string(builder, 0, 8) == "builtin:";
}


StorePath writeDerivation(ref<Store> store,
    const Derivation & drv, std::string_view name, RepairFlag repair)
{
    auto references = drv.inputSrcs;
    for (auto & i : drv.inputDrvs)
        references.insert(i.first);
    /* Note that the outputs of a derivation are *not* references
       (that can be missing (of course) and should not necessarily be
       held during a garbage collection). */
    auto suffix = std::string(name) + drvExtension;
    auto contents = drv.unparse(*store, false);
    return settings.readOnlyMode
        ? store->computeStorePathForText(suffix, contents, references)
        : store->addTextToStore(suffix, contents, references, repair);
}


/* Read string `s' from stream `str'. */
static void expect(std::istream & str, const string & s)
{
    char s2[s.size()];
    str.read(s2, s.size());
    if (string(s2, s.size()) != s)
        throw FormatError("expected string '%1%'", s);
}


/* Read a C-style string from stream `str'. */
static string parseString(std::istream & str)
{
    string res;
    expect(str, "\"");
    int c;
    while ((c = str.get()) != '"')
        if (c == '\\') {
            c = str.get();
            if (c == 'n') res += '\n';
            else if (c == 'r') res += '\r';
            else if (c == 't') res += '\t';
            else res += c;
        }
        else res += c;
    return res;
}


static Path parsePath(std::istream & str)
{
    string s = parseString(str);
    if (s.size() == 0 || s[0] != '/')
        throw FormatError("bad path '%1%' in derivation", s);
    return s;
}


static bool endOfList(std::istream & str)
{
    if (str.peek() == ',') {
        str.get();
        return false;
    }
    if (str.peek() == ']') {
        str.get();
        return true;
    }
    return false;
}


static StringSet parseStrings(std::istream & str, bool arePaths)
{
    StringSet res;
    while (!endOfList(str))
        res.insert(arePaths ? parsePath(str) : parseString(str));
    return res;
}


static DerivationOutput parseDerivationOutput(const Store & store, istringstream_nocopy & str)
{
    expect(str, ","); auto path = store.parseStorePath(parsePath(str));
    expect(str, ","); auto hashAlgo = parseString(str);
    expect(str, ","); const auto hash = parseString(str);
    expect(str, ")");

    if (hashAlgo != "") {
        auto method = FileIngestionMethod::Flat;
        if (string(hashAlgo, 0, 2) == "r:") {
            method = FileIngestionMethod::Recursive;
            hashAlgo = string(hashAlgo, 2);
        }
        const HashType hashType = parseHashType(hashAlgo);

        return DerivationOutput {
            .output = DerivationOutputFixed {
                .hash = FixedOutputHash {
                    .method = std::move(method),
                    .hash = Hash(hash, hashType),
                },
            }
        };
    } else
        return DerivationOutput {
            .output = DerivationOutputIntensional {
                .path = std::move(path),
            }
        };
}


static Derivation parseDerivation(const Store & store, const string & s, string name)
{
    Derivation drv;
    drv.name = name;

    istringstream_nocopy str(s);
    expect(str, "Derive([");

    /* Parse the list of outputs. */
    while (!endOfList(str)) {
        expect(str, "("); std::string id = parseString(str);
        auto output = parseDerivationOutput(store, str);
        drv.outputs.emplace(std::move(id), std::move(output));
    }

    /* Parse the list of input derivations. */
    expect(str, ",[");
    while (!endOfList(str)) {
        expect(str, "(");
        Path drvPath = parsePath(str);
        expect(str, ",[");
        drv.inputDrvs.insert_or_assign(store.parseStorePath(drvPath), parseStrings(str, false));
        expect(str, ")");
    }

    expect(str, ",["); drv.inputSrcs = store.parseStorePathSet(parseStrings(str, true));
    expect(str, ","); drv.platform = parseString(str);
    expect(str, ","); drv.builder = parseString(str);

    /* Parse the builder arguments. */
    expect(str, ",[");
    while (!endOfList(str))
        drv.args.push_back(parseString(str));

    /* Parse the environment variables. */
    expect(str, ",[");
    while (!endOfList(str)) {
        expect(str, "("); string name = parseString(str);
        expect(str, ","); string value = parseString(str);
        expect(str, ")");
        drv.env[name] = value;
    }

    expect(str, ")");
    return drv;
}


Derivation readDerivation(const Store & store, const Path & drvPath, std::string name)
{
    try {
        return parseDerivation(store, readFile(drvPath), name);
    } catch (FormatError & e) {
        throw Error("error parsing derivation '%1%': %2%", drvPath, e.msg());
    }
}


Derivation Store::derivationFromPath(const StorePath & drvPath)
{
    ensurePath(drvPath);
    return readDerivation(drvPath);
}


Derivation Store::readDerivation(const StorePath & drvPath)
{
    auto accessor = getFSAccessor();
    try {
        return parseDerivation(*this, accessor->readFile(printStorePath(drvPath)), std::string(drvPath.name()));
    } catch (FormatError & e) {
        throw Error("error parsing derivation '%s': %s", printStorePath(drvPath), e.msg());
    }
}


static void printString(string & res, std::string_view s)
{
    char buf[s.size() * 2 + 2];
    char * p = buf;
    *p++ = '"';
    for (auto c : s)
        if (c == '\"' || c == '\\') { *p++ = '\\'; *p++ = c; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\r') { *p++ = '\\'; *p++ = 'r'; }
        else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
        else *p++ = c;
    *p++ = '"';
    res.append(buf, p - buf);
}


static void printUnquotedString(string & res, std::string_view s)
{
    res += '"';
    res.append(s);
    res += '"';
}


template<class ForwardIterator>
static void printStrings(string & res, ForwardIterator i, ForwardIterator j)
{
    res += '[';
    bool first = true;
    for ( ; i != j; ++i) {
        if (first) first = false; else res += ',';
        printString(res, *i);
    }
    res += ']';
}


template<class ForwardIterator>
static void printUnquotedStrings(string & res, ForwardIterator i, ForwardIterator j)
{
    res += '[';
    bool first = true;
    for ( ; i != j; ++i) {
        if (first) first = false; else res += ',';
        printUnquotedString(res, *i);
    }
    res += ']';
}


string Derivation::unparse(const Store & store, bool maskOutputs,
    std::map<std::string, StringSet> * actualInputs) const
{
    string s;
    s.reserve(65536);
    s += "Derive([";

    bool first = true;
    for (auto & i : outputs) {
        if (first) first = false; else s += ',';
        s += '('; printUnquotedString(s, i.first);
        s += ','; printUnquotedString(s, maskOutputs ? "" : store.printStorePath(i.second.path(store, name)));
        if (auto hash = std::get_if<DerivationOutputFixed>(&i.second.output)) {
            s += ','; printUnquotedString(s, hash->hash.printMethodAlgo());
            s += ','; printUnquotedString(s, hash->hash.hash.to_string(Base16, false));
        } else {
            s += ','; printUnquotedString(s, "");
            s += ','; printUnquotedString(s, "");
        }
        s += ')';
    }

    s += "],[";
    first = true;
    if (actualInputs) {
        for (auto & i : *actualInputs) {
            if (first) first = false; else s += ',';
            s += '('; printUnquotedString(s, i.first);
            s += ','; printUnquotedStrings(s, i.second.begin(), i.second.end());
            s += ')';
        }
    } else {
        for (auto & i : inputDrvs) {
            if (first) first = false; else s += ',';
            s += '('; printUnquotedString(s, store.printStorePath(i.first));
            s += ','; printUnquotedStrings(s, i.second.begin(), i.second.end());
            s += ')';
        }
    }

    s += "],";
    auto paths = store.printStorePathSet(inputSrcs); // FIXME: slow
    printUnquotedStrings(s, paths.begin(), paths.end());

    s += ','; printUnquotedString(s, platform);
    s += ','; printString(s, builder);
    s += ','; printStrings(s, args.begin(), args.end());

    s += ",[";
    first = true;
    for (auto & i : env) {
        if (first) first = false; else s += ',';
        s += '('; printString(s, i.first);
        s += ','; printString(s, maskOutputs && outputs.count(i.first) ? "" : i.second);
        s += ')';
    }

    s += "])";

    return s;
}


// FIXME: remove
bool isDerivation(const string & fileName)
{
    return hasSuffix(fileName, drvExtension);
}


bool BasicDerivation::isFixedOutput() const
{
    return outputs.size() == 1 &&
        outputs.begin()->first == "out" &&
        std::holds_alternative<DerivationOutputFixed>(outputs.begin()->second.output);
}


DrvHashes drvHashes;


/* Returns the hash of a derivation modulo fixed-output
   subderivations.  A fixed-output derivation is a derivation with one
   output (`out') for which an expected hash and hash algorithm are
   specified (using the `outputHash' and `outputHashAlgo'
   attributes).  We don't want changes to such derivations to
   propagate upwards through the dependency graph, changing output
   paths everywhere.

   For instance, if we change the url in a call to the `fetchurl'
   function, we do not want to rebuild everything depending on it
   (after all, (the hash of) the file being downloaded is unchanged).
   So the *output paths* should not change.  On the other hand, the
   *derivation paths* should change to reflect the new dependency
   graph.

   That's what this function does: it returns a hash which is just the
   hash of the derivation ATerm, except that any input derivation
   paths have been replaced by the result of a recursive call to this
   function, and that for fixed-output derivations we return a hash of
   its output path. */
Hash hashDerivationModulo(Store & store, const Derivation & drv, bool maskOutputs)
{
    /* Return a fixed hash for fixed-output derivations. */
    if (drv.isFixedOutput()) {
        DerivationOutputs::const_iterator i = drv.outputs.begin();
        auto hash = std::get<DerivationOutputFixed>(i->second.output);
        return hashString(htSHA256, "fixed:out:"
            + hash.hash.printMethodAlgo() + ":"
            + hash.hash.hash.to_string(Base16, false) + ":"
            + store.printStorePath(i->second.path(store, drv.name)));
    }

    /* For other derivations, replace the inputs paths with recursive
       calls to this function.*/
    std::map<std::string, StringSet> inputs2;
    for (auto & i : drv.inputDrvs) {
        auto h = drvHashes.find(i.first);
        if (h == drvHashes.end()) {
            assert(store.isValidPath(i.first));
            h = drvHashes.insert_or_assign(i.first, hashDerivationModulo(store,
                    store.readDerivation(i.first), false)).first;
        }
        inputs2.insert_or_assign(h->second.to_string(Base16, false), i.second);
    }

    return hashString(htSHA256, drv.unparse(store, maskOutputs, &inputs2));
}


std::string StorePathWithOutputs::to_string(const Store & store) const
{
    return outputs.empty()
        ? store.printStorePath(path)
        : store.printStorePath(path) + "!" + concatStringsSep(",", outputs);
}


bool wantOutput(const string & output, const std::set<string> & wanted)
{
    return wanted.empty() || wanted.find(output) != wanted.end();
}


StorePathSet BasicDerivation::outputPaths(const Store & store) const
{
    StorePathSet paths;
    for (auto & i : outputs)
        paths.insert(i.second.path(store, name));
    return paths;
}

static DerivationOutput readDerivationOutput(Source & in, const Store & store)
{
    auto path = store.parseStorePath(readString(in));
    auto hashAlgo = readString(in);
    auto hash = readString(in);

    if (hashAlgo != "") {
        auto method = FileIngestionMethod::Flat;
        if (string(hashAlgo, 0, 2) == "r:") {
            method = FileIngestionMethod::Recursive;
            hashAlgo = string(hashAlgo, 2);
        }
        auto hashType = parseHashType(hashAlgo);
        return DerivationOutput {
            .output = DerivationOutputFixed {
                .hash = FixedOutputHash {
                    .method = std::move(method),
                    .hash = Hash(hash, hashType),
                },
            }
        };
    } else
        return DerivationOutput {
            .output = DerivationOutputIntensional {
                .path = std::move(path),
            }
        };
}

StringSet BasicDerivation::outputNames() const
{
    StringSet names;
    for (auto & i : outputs)
        names.insert(i.first);
    return names;
}


Source & readDerivation(Source & in, const Store & store, BasicDerivation & drv, string name)
{
    drv.name = name;

    drv.outputs.clear();
    auto nr = readNum<size_t>(in);
    for (size_t n = 0; n < nr; n++) {
        auto name = readString(in);
        auto output = readDerivationOutput(in, store);
        drv.outputs.emplace(std::move(name), std::move(output));
    }

    drv.inputSrcs = readStorePaths<StorePathSet>(store, in);
    in >> drv.platform >> drv.builder;
    drv.args = readStrings<Strings>(in);

    nr = readNum<size_t>(in);
    for (size_t n = 0; n < nr; n++) {
        auto key = readString(in);
        auto value = readString(in);
        drv.env[key] = value;
    }

    return in;
}


void writeDerivation(Sink & out, const Store & store, const BasicDerivation & drv)
{
    out << drv.outputs.size();
    for (auto & i : drv.outputs) {
        out << i.first
            << store.printStorePath(i.second.path(store, drv.name));
        if (auto hash = std::get_if<DerivationOutputFixed>(&i.second.output)) {
            out << hash->hash.printMethodAlgo()
                << hash->hash.hash.to_string(Base16, false);
        } else {
            out << "" << "";
        }
    }
    writeStorePaths(store, out, drv.inputSrcs);
    out << drv.platform << drv.builder << drv.args;
    out << drv.env.size();
    for (auto & i : drv.env)
        out << i.first << i.second;
}


std::string hashPlaceholder(const std::string & outputName)
{
    // FIXME: memoize?
    return "/" + hashString(htSHA256, "nix-output:" + outputName).to_string(Base32, false);
}


}