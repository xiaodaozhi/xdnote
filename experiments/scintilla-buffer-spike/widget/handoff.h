#pragma once

class FixtureScintillaEdit;

enum class FixtureHandoff { Completed, Composing, Failed };

// Test-only candidate: both widgets must already reference the same document.
FixtureHandoff transferFixture(FixtureScintillaEdit &from, FixtureScintillaEdit &to);
