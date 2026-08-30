"use strict";
/* tb-tray reporter — минималистичная версия */

const ENDPOINT = "http://127.0.0.1:8765/report";

async function report() {
    let stage = "start";
    try {
        stage = "query";
        let unread = 0;
        const boxes = {};
        let page = await browser.messages.query({ unread: true });
        const all = page.messages.slice();
        while (page.id) {
            page = await browser.messages.continueList(page.id);
            all.push(...page.messages);
        }
        for (const m of all) {
            unread++;
            const key = (m.folder && m.folder.name) ? m.folder.name : "?";
            boxes[key] = (boxes[key] || 0) + 1;
            last = { from: m.author, subject: m.subject };
        }
        stage = "fetch";
        const st = await browser.storage.local.get("closeAction");
        const payload = {
            unread: unread,
            last: last,
            closeAction: st.closeAction || "hide",
            boxes: Object.keys(boxes).map(k => ({ name: k, unread: boxes[k] }))
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

let last = { from: "", subject: "" };

browser.messages.onNewMailReceived.addListener((folder, messages) => {
    report();
});

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
