"use strict";
/*
 * tb-tray reporter: counts unread messages and reports them to the
 * tb-tray GTK tray applet listening on 127.0.0.1:8765.
 */

const ENDPOINT = "http://127.0.0.1:8765/report";

async function report() {
    try {
        let unread = 0;
        let last = null;

        let page = await browser.messages.query({ unread: true });
        unread += page.messages.length;
        if (page.messages.length) {
            const m = page.messages[page.messages.length - 1];
            last = { from: m.author, subject: m.subject };
        }
        while (page.id) {
            page = await browser.messages.continueList(page.id);
            unread += page.messages.length;
            if (page.messages.length) {
                const m = page.messages[page.messages.length - 1];
                last = { from: m.author, subject: m.subject };
            }
        }

        await fetch(ENDPOINT, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ unread: unread, last: last })
        });
    } catch (e) {
        /* applet not running — silent */
    }
}

browser.messages.onNewMailReceived.addListener((folder, messages) => {
    report();
});

browser.alarms.create("tb-tray-report", { periodInMinutes: 1 });
browser.alarms.onAlarm.addListener(a => {
    if (a.name === "tb-tray-report") report();
});

report();
