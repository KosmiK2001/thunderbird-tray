"use strict";
/*
 * tb-tray reporter: counts unread messages (all accounts) and reports them
 * to the tb-tray GTK tray applet on 127.0.0.1:8765.
 */

const ENDPOINT = "http://127.0.0.1:8765/report";

async function report() {
    let unread = -1, boxes = [], last = null, stage = "start";
    try {
        stage = "accounts";
        const accounts = await browser.accounts.list();
        for (const acc of accounts) {
            let aUnread = 0, aTotal = 0;
            const stack = [...(acc.folders || [])];
            while (stack.length) {
                const f = stack.pop();
                (f.subFolders || []).forEach(s => stack.push(s));
                try {
                    stage = "getFolderInfo " + f.id;
                    const info = await browser.folders.getFolderInfo(f);
                    aUnread += info.totalUnread || 0;
                    aTotal += info.totalMessageCounts || 0;
                } catch (e) { /* folder vanished */ }
            }
            boxes.push({ name: acc.name || acc.id, unread: aUnread, total: aTotal });
            unread += aUnread;
        }

        stage = "last-unread-query";
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

        stage = "fetch";
        const st = await browser.storage.local.get("closeAction");
        const payload = {
            unread: unread,
            last: last,
            boxes: boxes,
            closeAction: st.closeAction || "hide"
        };
        await fetch(ENDPOINT, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload)
        });
        console.log("tb-tray: reported OK, unread =", unread);
    } catch (e) {
        console.error("tb-tray: report failed at", stage, e);
        try {
            await fetch(ENDPOINT, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ unread: -1, error: stage + ": " + (e && e.message ? e.message : String(e)) })
            });
        } catch (e2) { /* applet недоступен */ }
    }
}

browser.messages.onNewMailReceived.addListener((folder, messages) => {
    report();
});

/* alarms может отсутствовать в некоторых сборках — дублируем таймером */
if (browser.alarms) {
    browser.alarms.create("tb-tray-report", { periodInMinutes: 1 });
    browser.alarms.onAlarm.addListener(a => {
        if (a.name === "tb-tray-report") report();
    });
} else {
    setInterval(report, 60000);
}

console.log("tb-tray reporter: background started");
report();
