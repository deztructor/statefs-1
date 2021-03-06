/**
 * @file config.hpp
 * @brief Statefs configuration access implementation
 *
 * @author (C) 2012, 2013 Jolla Ltd. Denis Zalevskiy <denis.zalevskiy@jollamobile.com>
 * @copyright LGPL 2.1 http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include "statefs.hpp"
#include "config.hpp"

#include <statefs/provider.h>
#include <statefs/util.h>

#include <cor/so.hpp>
#include <cor/util.h>
#include <cor/util.hpp>

#include <sys/eventfd.h>

#include <iostream>

#include <stdio.h>
#include <stdbool.h>
#include <poll.h>

namespace statefs { namespace config {

using statefs::server::LoadersStorage;
using statefs::server::LoaderProxy;

template <typename ProviderFn, typename LoaderFn>
void process_lib_info(std::shared_ptr<Library> p
                      , ProviderFn on_provider
                      , LoaderFn on_loader)
{
    if (!p) {
        std::cerr << "process_lib_info: nullptr" << std::endl;
        return;
    }

    auto loader = std::dynamic_pointer_cast<Loader>(p);
    if (loader) {
        on_loader(loader);
        return;
    }
    auto provider = std::dynamic_pointer_cast<Plugin>(p);
    if (!provider)
        throw cor::Error("Unknown lib: %s", p->value().c_str());
    on_provider(provider);
}

namespace fs = boost::filesystem;

static const std::set<std::string> cfg_prefices
= {cfg_provider_prefix(), cfg_loader_prefix()};

static std::string filename_prefix(std::string const& path)
{
    auto n = path.find("-");
    return (n != std::string::npos) ? path.substr(0, n) : "";
}

static bool is_config_file(fs::path const& path)
{
    if (path.extension() != cfg_extension())
        return false;

    auto prefix = filename_prefix(path.filename().string());
    return (cfg_prefices.count(prefix) > 0);
}

static bool is_config_file(std::string const& fname)
{
    fs::path path(fname);
    return is_config_file(path);
}

bool from_file(std::string const &cfg_src, config_receiver_fn receiver)
{
    trace() << "Loading config from " << cfg_src << std::endl;
    std::ifstream input(cfg_src);
    try {
        using namespace std::placeholders;
        parse(input, std::bind(receiver, cfg_src, _1));
    } catch (...) {
        std::cerr << "Error parsing " << cfg_src << ":" << input.tellg()
                 << ", skiping..." << std::endl;
        return false;
    }
    return true;
}

bool check_name_load(std::string const &cfg_src, config_receiver_fn receiver)
{
    if (!is_config_file(cfg_src)) {
        std::cerr << "File " << cfg_src
                  << " is not config?, skipping" << std::endl;
        return false;
    }
    return from_file(cfg_src, receiver);
}

template <typename ReceiverT>
void from_dir(std::string const &cfg_src, ReceiverT receiver)
{
    trace() << "Config dir " << cfg_src << std::endl;
    auto check_load = [&receiver](fs::directory_entry const &d) {
        if (!fs::is_directory(d.path()))
            check_name_load(d.path().string(), receiver);
    };
    std::for_each(fs::directory_iterator(cfg_src),
                  fs::directory_iterator(),
                  check_load);
}

template <typename ReceiverT>
void load_from(std::string const &cfg_src, ReceiverT receiver)
{
    if (cfg_src.empty())
        return;

    if (fs::is_regular_file(cfg_src))
        check_name_load(cfg_src, receiver);
    else if (fs::is_directory(cfg_src))
        return from_dir(cfg_src, receiver);
    else
        throw cor::Error("Unknown configuration source %s", cfg_src.c_str());
}

void visit(std::string const &path, config_receiver_fn fn)
{
    load_from(path, fn);
}

class Loaders : public LoadersStorage
{
public:
    Loaders(std::string const &src);
};

Loaders::Loaders(std::string const &src)
{
    using namespace std::placeholders;
    auto lib_add = [this](std::string const &cfg_path
                          , std::shared_ptr<Library> p) {
        auto dummy = [](std::shared_ptr<Plugin>) { return; };
        process_lib_info
        (p, dummy, std::bind(&LoadersStorage::loader_register, this, _1));
    };
    load_from(src, lib_add);
}


template <typename CharT>
std::basic_ostream<CharT> & operator <<
(std::basic_ostream<CharT> &out, Property const &src)
{
    out << "\n";
    out << "(" << "prop" << " \"" << src.value() << "\" "
         << " \""<< src.defval() << "\" ";
    unsigned access = src.access();
    if (!(access & Property::Subscribe))
        out << " :behavior continuous";
    if (access & Property::Write) {
        if (access & Property::Read)
            out << " :access rw";
        else
            out << " :access wonly";
    }
    out << ")";

    return out;
}

template <typename CharT>
std::basic_ostream<CharT> & operator <<
(std::basic_ostream<CharT> &out, Namespace const &src)
{
    out << "\n";
    out << "(" << "ns" << " \"" << src.value() << "\"";
    for(auto &prop: src.props_)
        out << *prop;
    out << ")";

    return out;
}

template <typename CharT>
std::basic_ostream<CharT> & operator <<
(std::basic_ostream<CharT> &out, Plugin const &src)
{
    out << "(" << "provider" << " \"" << src.value() << "\"";

    out << " \"" << src.path << "\"";
    for (auto &prop: src.info_)
        out << " :" << prop.first << " " << to_nl_string(prop.second);
    for(auto &ns: src.namespaces_)
        out << *ns;
    out << ")\n";

    return out;
}

template <typename CharT>
std::basic_ostream<CharT> & operator <<
(std::basic_ostream<CharT> &out, Loader const &src)
{
    out << "(" << "loader" << " \"" << src.value() << "\"";

    out << " \"" << src.path << "\"";
    out << ")\n";

    return out;
}

template <typename CharT>
std::basic_ostream<CharT> & operator <<
(std::basic_ostream<CharT> &out, std::shared_ptr<Library> const &src)
{
    auto prov = [&out](std::shared_ptr<Plugin> p) { out << *p; };
    auto loader = [&out](std::shared_ptr<Loader> p) { out << *p; };
    process_lib_info(src, prov, loader);
    return out;
}

static property_type statefs_variant_2prop(struct statefs_variant const *src)
{
    property_type res;
    switch (src->tag)
    {
    case statefs_variant_int:
        res = src->i;
        break;
    case statefs_variant_uint:
        res = src->u;
        break;
    case statefs_variant_bool:
        res = (long)src->b;
        break;
    case statefs_variant_real:
        res = src->r;
        break;
    case statefs_variant_cstr:
        res = src->s;
        break;
    default:
        res = "";
    }
    return res;
}


namespace nl = cor::notlisp;

void to_property(nl::expr_ptr expr, property_type &dst)
{
    if (!expr)
        throw cor::Error("to_property: Null");

    switch(expr->type()) {
    case nl::Expr::String:
        dst = expr->value();
        break;
    case nl::Expr::Integer:
        dst = (long)*expr;
        break;
    case nl::Expr::Real:
        dst = (double)*expr;
        break;
    default:
        throw cor::Error("%s is not compatible with Any",
                         expr->value().c_str());
    }
}

struct AnyToString : public boost::static_visitor<>
{
    std::string &dst;

    AnyToString(std::string &res) : dst(res) {}

    void operator () (std::string const &v) const
    {
        dst = v;
    }

    template <typename T>
    void operator () (T const &v) const
    {
        dst = std::to_string(v);
    }
};

std::string to_string(property_type const &p)
{
    std::string res;
    boost::apply_visitor(AnyToString(res), p);
    return res;
}

/// convert property_type to notlisp printable value
struct AnyToOutString : public boost::static_visitor<>
{
    std::string &dst;

    AnyToOutString(std::string &res);

    void operator () (std::string const &v) const;

    template <typename T>
    void operator () (T const &v) const
    {
        dst = std::to_string(v);
    }
};

AnyToOutString::AnyToOutString(std::string &res) : dst(res) {}

void AnyToOutString::operator () (std::string const &v) const
{
    dst = "\"";
    dst += (v + "\"");
}

std::string to_nl_string(property_type const &p)
{
    std::string res;
    boost::apply_visitor(AnyToOutString(res), p);
    return res;
}

Property::Property(std::string const &name,
                   property_type const &defval,
                   unsigned access)
    : ObjectExpr(name), defval_(defval), access_(access)
{}

Namespace::Namespace(std::string const &name, storage_type &&props)
    : ObjectExpr(name), props_(props)
{}

Library::Library(std::string const &name, std::string const &so_path)
    : ObjectExpr(name), path(so_path)
{
}

Plugin::Plugin(std::string const &name
               , std::string const &path
               , property_map_type &&info
               , storage_type &&namespaces)
    : Library(name, path)
    , mtime_(fs::exists(path) ? fs::last_write_time(path) : 0)
    , info_(info)
    , namespaces_(namespaces)
{}

Loader::Loader(std::string const &name, std::string const &path)
    : Library(name, path)
{}

struct PropertyInt : public boost::static_visitor<>
{
    long &dst;

    PropertyInt(long &res) : dst(res) {}

    void operator () (long v) const
    {
        dst = v;
    }

    template <typename OtherT>
    void operator () (OtherT &v) const
    {
        throw cor::Error("Wrong property type");
    }
};

long to_integer(property_type const &src)
{
    long res;
    boost::apply_visitor(PropertyInt(res), src);
    return res;
}

std::string Property::defval() const
{
    return to_string(defval_);
}

int Property::mode(int umask) const
{
    int res = 0;
    if (access_ & Read)
        res |= 0444;
    if (access_ & Write)
        res |= 0222;
    return res & ~umask;
}

static bool set_property(property_map_type &dst
                         , std::string const &name
                         , nl::expr_ptr const &v)
{
    auto it = dst.find(name);
    if (it != dst.end()) {
        to_property(v, it->second);
        return true;
    }
    std::cerr << "Unknown property " << name << std::endl;
    return false;
};

nl::env_ptr mk_parse_env()
{
    using nl::env_ptr;
    using nl::expr_list_type;
    using nl::lambda_type;
    using nl::expr_ptr;
    using nl::ListAccessor;
    using nl::to_string;

    lambda_type plugin = [](env_ptr, expr_list_type &params) {
        ListAccessor src(params);
        std::string name, path;
        src.required(to_string, name).required(to_string, path);

        Plugin::storage_type namespaces;
        property_map_type options = {
            // default option values
            {"type", "default"}
        };
        auto add_ns = [&namespaces](expr_ptr &v) {
            auto ns = std::dynamic_pointer_cast<Namespace>(v);
            if (!ns)
                throw cor::Error("Can't be casted to Namespace");
            namespaces.push_back(ns);
        };
        auto set_option = [&options](expr_ptr const &k, expr_ptr const &v) {
            set_property(options, k->value(), v);
        };
        nl::rest(src, add_ns, set_option);
        return nl::expr_ptr(new Plugin(name, path
                                       , std::move(options)
                                       , std::move(namespaces)));
    };

    lambda_type prop = [](env_ptr, expr_list_type &params) {
        ListAccessor src(params);
        std::string name;
        property_type defval;
        src.required(to_string, name).required(to_property, defval);

        property_map_type options = {
            // default option values
            {"behavior", "discrete"},
            {"access", (long)Property::Read}
        };
        auto add_option = [&options](expr_ptr const &k, expr_ptr const &v) {
            set_property(options, k->value(), v);
        };
        nl::rest(src, [](expr_ptr &) {}, add_option);
        unsigned access = to_integer(options["access"]);
        if (config::to_string(options["behavior"]) == "discrete")
                access |= Property::Subscribe;

        nl::expr_ptr res(new Property(name, defval, access));

        return res;
    };

    lambda_type ns = [](env_ptr, expr_list_type &params) {
        ListAccessor src(params);
        std::string name;
        src.required(to_string, name);

        Namespace::storage_type props;
        nl::push_rest_casted(src, props);
        nl::expr_ptr res(new Namespace(name, std::move(props)));
        return res;
    };

    lambda_type loader = [](env_ptr, expr_list_type &params) {
        ListAccessor src(params);
        std::string name, path;
        src.required(to_string, name).required(to_string, path);

        return expr_ptr(new Loader(name, path));
    };


    using nl::mk_record;
    using nl::mk_const;
    env_ptr env(new nl::Env({
                mk_record("provider", plugin),
                    mk_record("loader", loader),
                    mk_record("ns", ns),
                    mk_record("prop", prop),
                    mk_const("false", 0),
                    mk_const("true", 1),
                    mk_const("discrete", Property::Subscribe),
                    mk_const("continuous", 0),
                    mk_const("rw", Property::Write | Property::Read),
                    mk_const("wonly", Property::Write),
                    }));
    return env;
}

namespace inotify = cor::inotify;

Monitor::Monitor
(std::string const &path, ConfigReceiver &target)
    : path_([](std::string const &path) {
            trace() << "Config monitor for " << path << std::endl;
            if (!ensure_dir_exists(path))
                throw cor::Error("No config dir %s", path.c_str());
            return path;
        }(path))
    , target_(target)
{
    using namespace std::placeholders;
    auto add = std::bind(&Monitor::lib_add, this, _1, _2);
    config::load_from(path_, add);
}

Monitor::~Monitor()
{
}

void Monitor::lib_add(std::string const &cfg_path, Monitor::lib_ptr p)
{
    if (!p)
        return;

    auto lib_fname = p->path;
    if (!fs::exists(lib_fname)) {
        std::cerr << "Library " << lib_fname
                  << " doesn't exist, skipping" << std::endl;
        return;
    }
    auto fname = fs::path(cfg_path).filename().string();
    using namespace std::placeholders;
    auto provider_add = std::bind(&ConfigReceiver::provider_add, &target_, _1);
    auto loader_add = std::bind(&ConfigReceiver::loader_add, &target_, _1);
    process_lib_info(p , provider_add, loader_add);
}

static Namespace::prop_type from_api
(statefs_property const *prop, statefs_io &io)
{
    int attr = io.getattr(prop);
    auto defval = statefs_variant_2prop(&prop->default_value);
    unsigned access = 0;
    if (attr & STATEFS_ATTR_DISCRETE)
        access |= Property::Subscribe;
    if (attr & STATEFS_ATTR_WRITE)
        access |= Property::Write;
    if (attr & STATEFS_ATTR_READ)
        access |= Property::Read;
    return std::make_shared<Property>(prop->node.name, defval, access);
}

typedef cor::Handle<
    cor::GenericHandleTraits<
        intptr_t, 0> > branch_handle_type;

static Plugin::ns_type from_api
(statefs_namespace const *ns, statefs_io &io)
{
    auto ns_name = ns->node.name;

    branch_handle_type iter
        (statefs_first(&ns->branch),
         [&ns](intptr_t v) {
            statefs_branch_release(&ns->branch, v);
        });
    auto next = [&ns, &iter]() {
        return mk_property_handle
        (statefs_prop_get(&ns->branch, iter.value()));
    };
    auto prop = next();
    Namespace::storage_type props;
    while (prop) {
        props.push_back(from_api(prop.get(), io));
        statefs_next(&ns->branch, &iter.ref());
        prop = next();
    }
    return std::make_shared<Namespace>(ns_name, std::move(props));
}

static std::shared_ptr<Plugin> from_api
(statefs::provider_ptr provider_, std::string const& path
 , std::string const& provider_type)
{
    if (!provider_) {
        std::cerr << "Provider " << path << " is not loaded" << std::endl;
        return nullptr;
    }
    auto const &root = provider_->root;
    auto provider_name = root.node.name;
    property_map_type props = {{"type", provider_type}};
    auto info = root.node.info;
    if (info) {
        while (info->name) {
            props[info->name] = statefs_variant_2prop(&info->value);
            ++info;
        }
    }

    branch_handle_type iter
        (statefs_first(&root.branch),
         [&root](intptr_t v) {
            statefs_branch_release(&root.branch, v);
        });

    auto next = [&root, &iter]() {
        return mk_namespace_handle
        (statefs_ns_get(&root.branch, iter.value()));
    };
    auto pns = next();
    Plugin::storage_type namespaces;
    while (pns) {
        namespaces.push_back(from_api(pns.get(), provider_->io));
        statefs_next(&root.branch, &iter.ref());
        pns = next();
    }
    return std::make_shared<Plugin>
        (provider_name, path, std::move(props), std::move(namespaces));
}

static std::shared_ptr<Loader> from_api
(std::shared_ptr<LoaderProxy> loader_, std::string const& path)
{
    return loader_
        ? std::make_shared<Loader>(loader_->name(), path)
        : nullptr;
}

static std::string mk_provider_path(std::string const &path)
{
    namespace fs = boost::filesystem;
    auto provider_path = fs::path(path);
    provider_path = fs::canonical(provider_path);
    return provider_path.generic_string();
}

static inline std::string dump_provider_cfg_file
(std::ostream &dst, fs::path const &path)
{
    std::string plugin_name("");
    auto dump_plugin = [&dst, &plugin_name]
        (std::string const &cfg_path, Monitor::lib_ptr p) {
        if (p) {
             // special case: config file itself is a provider file
            if (!p->path.size())
                p->path = cfg_path;
            dst << p;
            plugin_name = p->value();
        }
    };
    if (check_name_load(path.native(), dump_plugin))
        return cfg_provider_prefix() + '-' + plugin_name;
    else
        return "";
}

static std::string dump_loader
(std::string const& cfg_dir, std::ostream &dst, fs::path const &path)
{
    auto loader = std::make_shared<LoaderProxy>(path.native());
    if (!loader->is_valid()) {
        std::cerr << "Not a loader: " << path.string() << std::endl;
        return dump_provider_cfg_file(dst, path);
    }

    auto loader_info = from_api(loader, path.native());
    dst << *loader_info;
    return cfg_loader_prefix() + "-" + loader_info->value();
}

static std::string dump_provider
(std::string const& cfg_dir, std::ostream &dst
 , fs::path const &path, std::string const& provider_type)
{
    std::cerr << "Trying to dump " << provider_type
              << " provider " << path << std::endl;
    Loaders loaders(cfg_dir);
    auto loader = loaders.loader_get(provider_type);
    if (!loader) {
        if (provider_type == "default") {
            std::cerr << "Trying to dump as a loader" << std::endl;
            return dump_loader(cfg_dir, dst, path);
        } else {
            std::cerr << "Can't find " << provider_type
                      << " loader" << std::endl;
            return "";
        }
    }

    auto prov_info = from_api
        (loader->load(path.native(), nullptr)
         , path.native(), provider_type);
    if (!prov_info) {
        std::cerr << "Not provider, trying loader\n";
        return dump_loader(cfg_dir, dst, path);
    }

    dst << *prov_info;
    return cfg_provider_prefix() + "-" + prov_info->value();
}

static inline std::string dump_
(std::string const& cfg_dir, std::ostream &dst
 , fs::path const &path, std::string const& provider_type)
{
    if (provider_type == "loader") {
        std::cerr << "Dumping loader " << path << std::endl;
        return dump_loader(cfg_dir, dst, path);
    }

    return dump_provider(cfg_dir, dst, path, provider_type);
}

/**
 * dumps configuration of provider/loader (shared library or conf
 * file). If configuration file is passed to the function it just
 * parse and dump it, if binary - extracts configuration information
 * in configuration file format
 *
 * @param dst output stream
 * @param path path to file to be introspected
 *
 * @return provider name
 */
std::string dump(std::string const &cfg_dir
                 , std::ostream &dst
                 , std::string const &path
                 , std::string const& provider_type)
{
    auto full_path = fs::path(mk_provider_path(path));
    return dump_(cfg_dir, dst, full_path, provider_type);
}

/**
 * saves provider/loader metadata in parsable/readable configuration
 * format
 *
 * @param cfg_dir destination directory
 * @param fname provider/info file name
 */
void save(std::string const &cfg_dir
          , std::string const &fname
          , std::string const& provider_type)
{
    namespace fs = boost::filesystem;

    std::stringstream ss;
    auto name = dump(cfg_dir, ss, fname, provider_type);
    if (name == "") {
        std::cerr << "Can't retrieve information from " << fname << std::endl;
        return;
    }
    std::cout << name << std::endl;

    auto cfg_path = fs::path(cfg_dir);
    cfg_path /= (name + cfg_extension());
    std::ofstream out(cfg_path.generic_string());
    out << ss.str();
    out.close();
    // touch configuration directory, be sure dir monitor watch will
    // observe changes
    std::time_t n = std::time(0);
    fs::last_write_time(cfg_dir, n);
}

void rm(std::string const &cfg_dir
        , std::string const &fname
        , std::string const& provider_type)
{
    auto full_path = mk_provider_path(fname);
    auto rm_config = [full_path](std::string const &cfg_path
                                 , std::shared_ptr<config::Library> p) {
        auto lib_fname = p->path;
        if (lib_fname == full_path && fs::exists(cfg_path))
            fs::remove(cfg_path);
    };
    config::visit(cfg_dir, rm_config);
}

}} // namespaces
