import assert from 'node:assert/strict';
import { test } from 'node:test';
import { borderlessSettingsOf, windowSettingsOf } from '../../src/mods/gameWindow';

/** A client's `DayZ.cfg` as the game writes it: CRLF, one `key=value;` a line. */
const WRITTEN_BY_THE_GAME = [
  'language="Russian";',
  'adapter=-1;',
  '3D_Performance=93750;',
  'Resolution_Bpp=32;',
  'WinX=312;',
  'WinY=160;',
  'WindowWidth=2560;',
  'WindowHeight=1440;',
  'Windowed=1;',
  'MSAA=0;',
  'VSync=1;',
  '',
].join('\r\n');

test('the settings of a client are filed under the Windows account, in the file of its build', () => {
  assert.equal(
    windowSettingsOf('P:\\Profiles\\CADCore\\client', 'Ilya', false),
    'P:\\Profiles\\CADCore\\client\\Users\\Ilya\\DayZ.cfg',
  );
  assert.equal(
    windowSettingsOf('P:\\Profiles\\CADCore\\client2', 'Ilya', true),
    'P:\\Profiles\\CADCore\\client2\\Users\\Ilya\\DayZ Exp.cfg',
  );
});

/**
 * The file a run leaves behind: a window as large as the monitor, at the corner the last run was
 * dragged to. What comes out asks for a window at the primary monitor's corner and names no
 * resolution — which the game reads as the desktop's, and so as its borderless mode — and keeps
 * everything else the developer set, in its place and in the game's own line endings.
 */
test('a window left at a stale corner comes out as a borderless one, and nothing else changes', () => {
  assert.equal(
    borderlessSettingsOf(WRITTEN_BY_THE_GAME),
    [
      'language="Russian";',
      'adapter=-1;',
      '3D_Performance=93750;',
      'Resolution_Bpp=32;',
      'WinX=0;',
      'WinY=0;',
      'Windowed=1;',
      'MSAA=0;',
      'VSync=1;',
      '',
    ].join('\r\n'),
  );
});

/** Put right once, a file is left alone: a launch writes nothing it would only write again. */
test('settings already asking for the borderless window come out as they went in', () => {
  const settled = borderlessSettingsOf(WRITTEN_BY_THE_GAME);

  assert.equal(borderlessSettingsOf(settled), settled);
});

/**
 * No file is exclusive fullscreen — a missing `Windowed` is read as zero — so the one a launch
 * makes says exactly what the window needs and nothing more.
 */
test('a profile with no settings yet gets a file asking for the borderless window', () => {
  assert.equal(borderlessSettingsOf(''), 'Windowed=1;\r\nWinX=0;\r\nWinY=0;\r\n');
});

/** Fullscreen is what `-window` used to stand in front of; the settings now say windowed instead. */
test('exclusive fullscreen asked for in the settings becomes the borderless window', () => {
  assert.equal(
    borderlessSettingsOf('language="English";\nWindowed=0;\n'),
    'language="English";\nWindowed=1;\nWinX=0;\nWinY=0;\n',
  );
});

/** The game matches keys whatever their case, so a key said twice or in another case is one key. */
test('a key in another case or said twice is written once, in the spelling the game uses', () => {
  assert.equal(
    borderlessSettingsOf('WINX=5;\r\nwinx=6;\r\nwindowwidth=1920;\r\n  WindowHeight = 1080;\r\n'),
    'WinX=0;\r\nWindowed=1;\r\nWinY=0;\r\n',
  );
});

/** A last line without its line ending is still a line, and the file ends with one either way. */
test('a file without a final line ending keeps its last setting', () => {
  assert.equal(
    borderlessSettingsOf('Windowed=1;\r\nWinX=0;\r\nWinY=0;\r\nFXAA=4;'),
    'Windowed=1;\r\nWinX=0;\r\nWinY=0;\r\nFXAA=4;\r\n',
  );
});
