const radios = document.querySelectorAll('input[name="close"]');

browser.storage.local.get("closeAction").then(v => {
    const cur = v.closeAction || "hide";
    for (const r of radios)
        if (r.value === cur) r.checked = true;
});

for (const r of radios)
    r.addEventListener("change", async () => {
        if (!r.checked) return;
        await browser.storage.local.set({ closeAction: r.value });
        document.getElementById("status").textContent = "Сохранено";
    });
