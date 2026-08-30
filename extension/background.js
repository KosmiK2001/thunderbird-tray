"use strict";
/*
 * tb-tray reporter: counts unread messages (all accounts) and reports them
 * to the tb-tray GTK tray applet on 127.0.0.1:8765.
 */

const ENDPOINT = "http://127.0.0.1:8765/report";

async function report() {
    try {
        let unread = 0;
        let last = null;
        let boxes = [];

        const accounts = await browser.accounts.list();
        for (const acc of accounts) {
            let aUnread = 0, aTotal = 0;
            const stack = [...(acc.folders || [])];
            while (stack.length) {
                const f = stack.pop();
                (f.subFolders || []).forEach(s => stack.push(s));
                try {
                    const info = await browser.folders.getFolderInfo(f);
                    aUnread += info.totalUnread || 0;
                    aTotal += info.totalMessageCounts || 0;
                } catch (e) { /* folder vanished */ }
            }
            boxes.push({ name: acc.name || acc.id, unread: aUnread, total: aTotal });
            unread += aUnread;
        }

        // последний непрочитанный (для уведомления)
        let page = await browser.messages.query({ unread: true });
        const all = page.messages.slice();
        while (page.id) {
            page = await browser.messages.continueList(page.id);
            all.push(...page.messages);
        }
        if (all.length) {
            const m = all[all.length - 1];
            last = { from: m.author, subject: m.subject };
        }

        await fetch(ENDPOINT, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ unread: unread, last: last, boxes: boxes })
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
