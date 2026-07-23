#include "Game.h"

void Game::drawMapEditor() {
    if (m_mapEditor) m_mapEditor->draw();
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
    }
}