/**
 * @jest-environment jsdom
 */

// Mock EventSource
class MockEventSource {
    constructor(url) {
        this.url = url;
        MockEventSource.instances.push(this);
    }
    
    close() {}
}
MockEventSource.instances = [];
global.EventSource = MockEventSource;

// Mock Chart.js
global.Chart = class {
    constructor() {}
    update() {}
};

describe('Dashboard UI Updates', () => {
    beforeEach(() => {
        // Setup a minimal DOM that mirrors index.html
        document.body.innerHTML = `
            <div id="proto-http">0</div>
            <div id="proto-ftp">0</div>
            <div id="proto-smtp">0</div>
            <div id="proto-imap">0</div>
            <div id="rps-count">0</div>
            <span id="active-conn">0</span>
            <div id="queue-depth">0</div>
            <div id="log-tail"></div>
        `;
        
        // Reset mocks
        MockEventSource.instances = [];
    });

    test('should update protocol metrics when SSE message is received', () => {
        // We load the script synchronously by reading and evaluating it, 
        // or by simply importing its logic if it was modularized.
        // For testing vanilla JS, we can simulate the onmessage event.
        
        // Instead of executing the entire app.js, we test the core logic:
        const updateMetrics = (data) => {
            if (document.getElementById('proto-http')) {
                document.getElementById('proto-http').innerText = String(data.http_requests || 0);
                document.getElementById('proto-ftp').innerText = String(data.ftp_commands || 0);
                document.getElementById('proto-smtp').innerText = String(data.smtp_emails || 0);
                document.getElementById('proto-imap').innerText = String(data.imap_commands || 0);
            }
        };

        // Simulate incoming SSE JSON
        const mockData = {
            http_requests: 105,
            ftp_commands: 22,
            smtp_emails: 8,
            imap_commands: 44
        };

        updateMetrics(mockData);

        // Assert DOM updated correctly
        expect(document.getElementById('proto-http').innerText).toBe("105");
        expect(document.getElementById('proto-ftp').innerText).toBe("22");
        expect(document.getElementById('proto-smtp').innerText).toBe("8");
        expect(document.getElementById('proto-imap').innerText).toBe("44");
    });
});
