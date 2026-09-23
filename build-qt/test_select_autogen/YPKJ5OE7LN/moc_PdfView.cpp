/****************************************************************************
** Meta object code from reading C++ file 'PdfView.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/ui/PdfView.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'PdfView.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN7PdfViewE_t {};
} // unnamed namespace

template <> constexpr inline auto PdfView::qt_create_metaobjectdata<qt_meta_tag_ZN7PdfViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PdfView",
        "selectionChanged",
        "",
        "reader::DocumentAnchor",
        "anchor",
        "objectClicked",
        "kind",
        "sourceActivated",
        "linkActivated",
        "page",
        "uri",
        "bookmarkRequested",
        "regionCaptured",
        "QImage",
        "image",
        "quickAskRequested",
        "seed",
        "pageChanged",
        "selectionGeometryReady",
        "zoomChanged",
        "zoom",
        "viewStateChanged",
        "scrollY",
        "rotation",
        "findRequested",
        "sidecarToggleRequested",
        "historyBackRequested",
        "historyForwardRequested"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'selectionChanged'
        QtMocHelpers::SignalData<void(const reader::DocumentAnchor &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'objectClicked'
        QtMocHelpers::SignalData<void(const reader::DocumentAnchor &, const QString &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { QMetaType::QString, 6 },
        }}),
        // Signal 'sourceActivated'
        QtMocHelpers::SignalData<void(const reader::DocumentAnchor &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'linkActivated'
        QtMocHelpers::SignalData<void(int, const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 }, { QMetaType::QString, 10 },
        }}),
        // Signal 'bookmarkRequested'
        QtMocHelpers::SignalData<void(const reader::DocumentAnchor &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'regionCaptured'
        QtMocHelpers::SignalData<void(const reader::DocumentAnchor &, const QImage &)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 13, 14 },
        }}),
        // Signal 'quickAskRequested'
        QtMocHelpers::SignalData<void(const QString &)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 16 },
        }}),
        // Signal 'pageChanged'
        QtMocHelpers::SignalData<void(int)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 },
        }}),
        // Signal 'selectionGeometryReady'
        QtMocHelpers::SignalData<void(int)>(18, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 },
        }}),
        // Signal 'zoomChanged'
        QtMocHelpers::SignalData<void(double)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 20 },
        }}),
        // Signal 'viewStateChanged'
        QtMocHelpers::SignalData<void(int, int, double, int)>(21, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 }, { QMetaType::Int, 22 }, { QMetaType::Double, 20 }, { QMetaType::Int, 23 },
        }}),
        // Signal 'findRequested'
        QtMocHelpers::SignalData<void()>(24, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'sidecarToggleRequested'
        QtMocHelpers::SignalData<void()>(25, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'historyBackRequested'
        QtMocHelpers::SignalData<void()>(26, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'historyForwardRequested'
        QtMocHelpers::SignalData<void()>(27, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PdfView, qt_meta_tag_ZN7PdfViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PdfView::staticMetaObject = { {
    QMetaObject::SuperData::link<QScrollArea::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7PdfViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7PdfViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7PdfViewE_t>.metaTypes,
    nullptr
} };

void PdfView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PdfView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->selectionChanged((*reinterpret_cast<std::add_pointer_t<reader::DocumentAnchor>>(_a[1]))); break;
        case 1: _t->objectClicked((*reinterpret_cast<std::add_pointer_t<reader::DocumentAnchor>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 2: _t->sourceActivated((*reinterpret_cast<std::add_pointer_t<reader::DocumentAnchor>>(_a[1]))); break;
        case 3: _t->linkActivated((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 4: _t->bookmarkRequested((*reinterpret_cast<std::add_pointer_t<reader::DocumentAnchor>>(_a[1]))); break;
        case 5: _t->regionCaptured((*reinterpret_cast<std::add_pointer_t<reader::DocumentAnchor>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QImage>>(_a[2]))); break;
        case 6: _t->quickAskRequested((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 7: _t->pageChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->selectionGeometryReady((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 9: _t->zoomChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 10: _t->viewStateChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[4]))); break;
        case 11: _t->findRequested(); break;
        case 12: _t->sidecarToggleRequested(); break;
        case 13: _t->historyBackRequested(); break;
        case 14: _t->historyForwardRequested(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const reader::DocumentAnchor & )>(_a, &PdfView::selectionChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const reader::DocumentAnchor & , const QString & )>(_a, &PdfView::objectClicked, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const reader::DocumentAnchor & )>(_a, &PdfView::sourceActivated, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(int , const QString & )>(_a, &PdfView::linkActivated, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const reader::DocumentAnchor & )>(_a, &PdfView::bookmarkRequested, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const reader::DocumentAnchor & , const QImage & )>(_a, &PdfView::regionCaptured, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(const QString & )>(_a, &PdfView::quickAskRequested, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(int )>(_a, &PdfView::pageChanged, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(int )>(_a, &PdfView::selectionGeometryReady, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(double )>(_a, &PdfView::zoomChanged, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)(int , int , double , int )>(_a, &PdfView::viewStateChanged, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)()>(_a, &PdfView::findRequested, 11))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)()>(_a, &PdfView::sidecarToggleRequested, 12))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)()>(_a, &PdfView::historyBackRequested, 13))
            return;
        if (QtMocHelpers::indexOfMethod<void (PdfView::*)()>(_a, &PdfView::historyForwardRequested, 14))
            return;
    }
}

const QMetaObject *PdfView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PdfView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7PdfViewE_t>.strings))
        return static_cast<void*>(this);
    return QScrollArea::qt_metacast(_clname);
}

int PdfView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QScrollArea::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void PdfView::selectionChanged(const reader::DocumentAnchor & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void PdfView::objectClicked(const reader::DocumentAnchor & _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void PdfView::sourceActivated(const reader::DocumentAnchor & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void PdfView::linkActivated(int _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}

// SIGNAL 4
void PdfView::bookmarkRequested(const reader::DocumentAnchor & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void PdfView::regionCaptured(const reader::DocumentAnchor & _t1, const QImage & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2);
}

// SIGNAL 6
void PdfView::quickAskRequested(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void PdfView::pageChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void PdfView::selectionGeometryReady(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}

// SIGNAL 9
void PdfView::zoomChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 9, nullptr, _t1);
}

// SIGNAL 10
void PdfView::viewStateChanged(int _t1, int _t2, double _t3, int _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 10, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 11
void PdfView::findRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}

// SIGNAL 12
void PdfView::sidecarToggleRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 12, nullptr);
}

// SIGNAL 13
void PdfView::historyBackRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 13, nullptr);
}

// SIGNAL 14
void PdfView::historyForwardRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 14, nullptr);
}
QT_WARNING_POP
