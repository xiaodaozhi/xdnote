#pragma once
namespace xdnote {
// Call before constructing QApplication in both the runner and its tests.
void initializeGuiRuntime();
// Call after QApplication exists and before creating application controls.
void configureGuiApplication();
}
