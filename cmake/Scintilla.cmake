set(XDNOTE_SCINTILLA_QT "${XDNOTE_SCINTILLA_SOURCE}/qt/ScintillaEditBase")
set(XDNOTE_SCINTILLA_NAMES
    AutoComplete CallTip CaseConvert CaseFolder CellBuffer ChangeHistory CharacterCategoryMap
    CharacterType CharClassify ContractionState DBCS Decoration Document EditModel Editor EditView
    Geometry Indicator KeyMap LineMarker MarginView PerLine PositionCache RESearch RunStyles
    ScintillaBase Selection Style UndoHistory UniConversion UniqueString ViewStyle XPM)
set(XDNOTE_SCINTILLA_SOURCES)
foreach(name IN LISTS XDNOTE_SCINTILLA_NAMES)
    list(APPEND XDNOTE_SCINTILLA_SOURCES "${XDNOTE_SCINTILLA_SOURCE}/src/${name}.cxx")
endforeach()
add_library(xdnote_scintilla STATIC ${XDNOTE_SCINTILLA_SOURCES}
    "${XDNOTE_SCINTILLA_QT}/PlatQt.cpp" "${XDNOTE_SCINTILLA_QT}/ScintillaQt.cpp"
    "${XDNOTE_SCINTILLA_QT}/ScintillaEditBase.cpp" "${XDNOTE_SCINTILLA_QT}/ScintillaEditBase.h")
target_compile_definitions(xdnote_scintilla PUBLIC SCINTILLA_QT=1 EXPORT_IMPORT_API=)
target_include_directories(xdnote_scintilla PUBLIC
    "${XDNOTE_SCINTILLA_SOURCE}/include" "${XDNOTE_SCINTILLA_QT}"
    # The upstream public Qt port header itself includes Debugging.h and Geometry.h.
    "${XDNOTE_SCINTILLA_SOURCE}/src")
target_link_libraries(xdnote_scintilla PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Core5Compat)
