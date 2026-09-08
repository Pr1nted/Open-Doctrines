#include "Game.h"
#include "GameInternals.h"
#include "MapEditor.h"

void Game::drawMapEditor() {
    if (!m_mapEditor) return;
    // Every frame, because Settings can change it while the editor is open and
    // the editor has no Config of its own to ask.
    MapEditor::setAccent(hexToColor(m_config.accent()));
    m_mapEditor->draw();
}

void Game::updateMapEditor() {
    if (m_mapEditor) {
        if (IsWindowResized()) {
            m_screenW = GetScreenWidth();
            m_screenH = GetScreenHeight();
            m_mapEditor->resize(m_screenW, m_screenH);
        }
        float dt = GetFrameTime();
        m_mapEditor->update(dt);

        // The editor decides when ESC means "leave" (its dispatcher closes
        // overlays/dialogs first and can show an unsaved-changes prompt).
        if (m_mapEditor->consumeExitRequest()) {
            m_currentScreen = SCREEN_MENU;
        }
        // The editor's Report button. Defaults to the Data category, which is
        // what a map bug almost always is; the player can change it.
        if (m_mapEditor->consumeFeedbackRequest()) {
            openFeedbackForm(feedback::Kind::Bug, feedback::Category::Data);
        }
    }
}