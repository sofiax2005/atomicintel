self.addEventListener('install', (event) => {
    event.waitUntil(self.skipWaiting());
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('sync', (event) => {
    if (event.tag === 'sync-attendance') {
        event.waitUntil(
            new Promise(async (resolve, reject) => {
                const dbRequest = indexedDB.open("AttendanceDB", 1);
                dbRequest.onsuccess = async () => {
                    const db = dbRequest.result;
                    const records = await getAllRecords(); // Assume getAllRecords from main JS, but duplicate here if needed
                    if (records.length === 0) return resolve();

                    try {
                        const response = await fetch(`${backendUrl}/sync`, {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify(records)
                        });
                        if (response.ok) {
                            records.forEach((rec, index) => deleteRecord(index + 1)); // Keys start from 1
                            console.log('Synced');
                            resolve();
                        } else {
                            reject();
                        }
                    } catch (e) {
                        reject(e);
                    }
                };
            })
        );
    }
});

// Duplicate getAllRecords for SW
function getAllRecords() {
    return new Promise((resolve, reject) => {
        const tx = db.transaction("records", "readonly");
        const request = tx.objectStore("records").getAll();
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject();
    });
}

// Duplicate deleteRecord for SW
function deleteRecord(key) {
    const tx = db.transaction("records", "readwrite");
    tx.objectStore("records").delete(key);
}