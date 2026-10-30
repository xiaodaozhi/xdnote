#pragma once

class ScintillaEditBase;

enum class FixtureHandoff { Completed, Composing, Failed };

// Test-only candidate: both widgets must already reference the same document.
FixtureHandoff transferFixture(ScintillaEditBase &from, ScintillaEditBase &to);
