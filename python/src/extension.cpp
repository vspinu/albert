// Copyright (c) 2017 Manuel Schneider

#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include "pythonmodulev1.h"
#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QMessageLogger>
#include <QMutexLocker>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>
#include <memory>
#include "xdg/iconlookup.h"
#include "core/query.h"
#include "core/item.h"
#include "core/action.h"
#include "util/standardactions.h"
#include "util/standarditem.h"
#include "extension.h"
#include "modulesmodel.h"
#include "configwidget.h"
#include "cast_specialization.h"
using namespace std;
using namespace Core;
namespace py = pybind11;

namespace Python {

const constexpr char* MODULES_DIR = "modules";
const constexpr char* CFG_ENABLEDMODS = "enabled_modules";

/*
 * Module definition
 */

#include <pybind11/stl.h>

PYBIND11_EMBEDDED_MODULE(albertv0, m)
{
    m.doc() = "pybind11 example module";

    py::class_<Core::Query, std::unique_ptr<Query, py::nodelete>>(m, "Query", "The query object to handle for a user input")
            .def_property_readonly("string", &Query::string)
            .def_property_readonly("rawString", &Query::rawString)
            .def_property_readonly("trigger", &Query::trigger)
            .def_property_readonly("isTriggered", &Query::isTriggered)
            .def_property_readonly("isValid", &Query::isValid)
            ;

    py::class_<Action, shared_ptr<Action>>(m, "ActionBase", "An abstract action")
            ;

    py::class_<Item, shared_ptr<Item>> iitem(m, "ItemBase", "An abstract item")
            ;

    py::class_<FuncAction, Action, shared_ptr<FuncAction>>(m, "FuncAction", "Executes the callable")
            .def(py::init([](QString text, const py::object& callable) {
                return std::make_shared<FuncAction>(move(text), [callable](){
                    try{ callable(); } catch (exception &e) { qWarning() << e.what(); }
                });
            }), py::arg("text"), py::arg("callable"))
            ;

    py::class_<ClipAction, Action, shared_ptr<ClipAction>>(m, "ClipAction", "Copies to clipboard")
            .def(py::init<QString, QString>(), py::arg("text"), py::arg("clipboardText"))
            ;

    py::class_<UrlAction, Action, shared_ptr<UrlAction>>(m, "UrlAction", "Opens a URL")
            .def(py::init<QString, QString>(), py::arg("text"), py::arg("url"))
            ;

    py::class_<ProcAction, Action, shared_ptr<ProcAction>>(m, "ProcAction", "Runs a process")
            .def(py::init([](QString text, list<QString> commandline, QString workdir) {
                return std::make_shared<ProcAction>(move(text), QStringList::fromStdList(commandline), move(workdir));
            }), py::arg("text"), py::arg("commandline"), py::arg("cwd") = QString())
            ;

    py::class_<TermAction, Action, shared_ptr<TermAction>>(m, "TermAction", "Runs a command in terminal")
            .def(py::init([](QString text, list<QString> commandline, QString workdir, bool shell) {
                return std::make_shared<TermAction>(move(text), QStringList::fromStdList(commandline), move(workdir), shell);
            }), py::arg("text"), py::arg("commandline"), py::arg("cwd") = QString(), py::arg("shell") = true)
            ;

    py::enum_<Item::Urgency>(iitem, "Urgency")
            .value("Alert", Item::Urgency::Alert)
            .value("Notification", Item::Urgency::Notification)
            .value("Normal", Item::Urgency::Normal)
            .export_values()
            ;

    py::class_<StandardItem, Item, shared_ptr<StandardItem>>(m, "Item", "A result item")
            .def(py::init<QString,QString,QString,QString,QString,Item::Urgency,vector<shared_ptr<Action>>>(),
                 py::arg("id") = QString(),
                 py::arg("icon") = QString(":python_module"),
                 py::arg("text") = QString(),
                 py::arg("subtext") = QString(),
                 py::arg("completion") = QString(),
                 py::arg("urgency") = Item::Urgency::Normal,
                 py::arg("actions") = vector<shared_ptr<Action>>())
            .def_property("id", &StandardItem::id, &StandardItem::setId)
            .def_property("icon", &StandardItem::iconPath, &StandardItem::setIconPath)
            .def_property("text", &StandardItem::text, &StandardItem::setText)
            .def_property("subtext", &StandardItem::subtext, &StandardItem::setSubtext)
            .def_property("completion", &StandardItem::completion, &StandardItem::setCompletion)
            .def_property("urgency", &StandardItem::urgency, &StandardItem::setUrgency)
            .def("addAction", &StandardItem::addAction)
            ;

    m.def("debug", [](const py::str &str){ qDebug() << str.cast<QString>(); });
    m.def("info", [](const py::str &str){ qInfo() << str.cast<QString>(); });
    m.def("warning", [](const py::str &str){ qWarning() << str.cast<QString>(); });
    m.def("critical", [](const py::str &str){ qCritical() << str.cast<QString>(); });
    m.def("iconLookup", [](const py::str &str){ return XDG::IconLookup::iconPath(str.cast<QString>()); });
}

}

class Python::Private
{
public:
    QPointer<ConfigWidget> widget;
    vector<unique_ptr<PythonModuleV1>> modules;
    QFileSystemWatcher fileSystemWatcher;
    py::object albert_module;
    QStringList enabledModules;
    QMutex pythonMutex;
};


/** ***************************************************************************/
Python::Extension::Extension()
    : Core::Extension("org.albert.extension.python"), // Must match the id in metadata
      Core::QueryHandler(Core::Plugin::id()),
      d(new Private) {

    if ( !Py_IsInitialized() )
        Py_InitializeEx(0);

    d->enabledModules = settings().value(CFG_ENABLEDMODS).toStringList();

    if ( !dataLocation().exists(MODULES_DIR) )
        dataLocation().mkdir(MODULES_DIR);

    try {
        d->albert_module = py::module::import("albertv0");
    } catch (exception &e) {
        throw e.what();
    }

    // Load the modules
    for (const QString &pluginDir : QStandardPaths::locateAll(QStandardPaths::DataLocation,
                                                              Core::Plugin::id(),
                                                              QStandardPaths::LocateDirectory) ) {
        QString extensionDir = QDir(pluginDir).filePath(MODULES_DIR);
        if ( QFile::exists(extensionDir) ) {
            try { // Append scriptsPath to sys.path
                py::module::import("sys").attr("path").cast<py::list>().append(extensionDir);
            } catch (exception &e) {
                throw e.what();
            }
            d->fileSystemWatcher.addPath(extensionDir);
            updateDirectory(extensionDir);
        }
    }

    connect(&d->fileSystemWatcher, &QFileSystemWatcher::directoryChanged,
            this, &Extension::updateDirectory);

    registerQueryHandler(this);
}



/** ***************************************************************************/
Python::Extension::~Extension() {
    d->modules.clear();
    d->albert_module.release();
}



/** ***************************************************************************/
QWidget *Python::Extension::widget(QWidget *parent) {
    if (d->widget.isNull()) {
        d->widget = new ConfigWidget(parent);

        ModulesModel *model = new ModulesModel(this, d->widget->ui.tableView);
        d->widget->ui.tableView->setModel(model);

        connect(d->widget->ui.tableView, &QTableView::activated,
                this, [this](const QModelIndex &index){
            QDesktopServices::openUrl(QUrl(d->modules[static_cast<size_t>(index.row())]->path()));
        });
    }
    return d->widget;
}


/** ***************************************************************************/
void Python::Extension::handleQuery(Core::Query *query) const {
    QMutexLocker lock(&d->pythonMutex);
    if ( query->isTriggered() ) {
        for ( auto & module : d->modules ) {
            if ( d->enabledModules.contains(module->id())
                 && module->state() == PythonModuleV1::State::Loaded
                 && module->trigger() == query->trigger() ) {
                module->handleQuery(query);
                return;
            }
        }
    }
    else {
        for ( auto & module : d->modules ) {
            if ( d->enabledModules.contains(module->id())
                 && module->state() == PythonModuleV1::State::Loaded
                 && module->trigger().isEmpty() ) {
                module->handleQuery(query);
                if ( !query->isValid() )
                    return;
            }
        }
    }
}


/** ***************************************************************************/
QStringList Python::Extension::triggers() const {
    QStringList retval;
    for ( auto &module : d->modules )
        retval << module->trigger();
    return retval;
}


/** ***************************************************************************/
const std::vector<std::unique_ptr<Python::PythonModuleV1> > &Python::Extension::modules() {
    return d->modules;
}


/** ***************************************************************************/
bool Python::Extension::isEnabled(Python::PythonModuleV1 &module) {
    return d->enabledModules.contains(module.id());
}


/** ***************************************************************************/
void Python::Extension::setEnabled(Python::PythonModuleV1 &module, bool enable) {
    if (enable)
        d->enabledModules.append(module.id());
    else
        d->enabledModules.removeAll(module.id());
    settings().setValue(CFG_ENABLEDMODS, d->enabledModules);
    enable ? module.load() : module.unload();
}


/** ***************************************************************************/
void Python::Extension::updateDirectory(const QString &path) {

    // Remove deleted modules, yes deletes in other dirs too whatever… it has been deleted
    for ( auto it = d->modules.begin(); it != d->modules.end(); ++it)
        if ( !QFile::exists((*it)->path()) )
             it = --d->modules.erase(it);

    // Add new modules
    QDirIterator dirIterator(path, QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    while (dirIterator.hasNext()) {
        QString path = dirIterator.next();
        QString id = dirIterator.fileInfo().completeBaseName();

        if (id == "__pycache__")
            continue;

        // Skip if this id already exists
        if ( find_if(d->modules.begin(), d->modules.end(),
                     [&id](const unique_ptr<PythonModuleV1> &rhs){return id == rhs->id();})
             != d->modules.end() )
            continue;

        PythonModuleV1 *module = new PythonModuleV1(path);
        d->modules.emplace_back(module);
        if (d->enabledModules.contains(module->id()))
            module->load();
        connect(module, &PythonModuleV1::moduleChanged,
                this, &Extension::modulesChanged);
    }

    std::sort(d->modules.begin(), d->modules.end(),
              [](auto& lhs, auto& rhs){ return 0 > lhs->name().localeAwareCompare(rhs->name()); });

    emit modulesChanged();
}

