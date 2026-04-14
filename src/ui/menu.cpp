#include "menu.h"
#include "display.h"
#include "button.h"

static const char* APP_NAMES[] = {
    "USB2TTL",
    "Servo Tester",
    "Motor DShot",
    "Battery Tool",
    "WiFi / Web",
    "CRSF Telem",
};

static int s_selected = 0;

void Menu::draw() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);

    // Title
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(15, 8);
    g->println("FPV MultiTool");

    // Divider
    g->drawFastHLine(0, 30, LCD_WIDTH, RGB565_DARKGREY);

    // Menu items
    g->setTextSize(2);
    int itemH = (LCD_HEIGHT - 50 - 20) / APP_COUNT;
    for (int i = 0; i < APP_COUNT; i++) {
        int y = 40 + i * itemH;

        if (i == s_selected) {
            g->fillRoundRect(4, y - 2, LCD_WIDTH - 8, itemH - 4, 6, RGB565_NAVY);
            g->setTextColor(RGB565_WHITE);
        } else {
            g->setTextColor(RGB565_DARKGREY);
        }

        g->setCursor(12, y + (itemH - 16) / 2);
        g->println(APP_NAMES[i]);
    }

    // Footer
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, LCD_HEIGHT - 14);
    g->print("Click=next  DblClick=prev  Hold=go");
}

AppId Menu::update(int btnEvent) {
    if (btnEvent == BTN_CLICK) {
        s_selected = (s_selected + 1) % APP_COUNT;
        draw();
    } else if (btnEvent == BTN_DOUBLE_CLICK) {
        s_selected = (s_selected - 1 + APP_COUNT) % APP_COUNT;
        draw();
    } else if (btnEvent == BTN_LONG_PRESS) {
        return (AppId)s_selected;
    }
    return APP_NONE;
}
