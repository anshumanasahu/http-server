import http from 'k6/http';
import { check, sleep } from 'k6';

// Export configuration options
export let options = {
    stages: [
        { duration: '30s', target: 200 },  // Ramp up to 200 users over 30 seconds
        { duration: '1m', target: 200 },   // Hold 200 users for 1 minute
        { duration: '30s', target: 500 },  // Spike to 500 users
        { duration: '30s', target: 0 },    // Ramp down to 0 users
    ],
};

export default function () {
    // We send a normal GET request to the HTTP server port
    let res = http.get('http://localhost:8080/');

    // Check that the request was successful and returned 200 OK
    check(res, {
        'status is 200': (r) => r.status === 200,
        'response time < 100ms': (r) => r.timings.duration < 100,
        'response time < 500ms': (r) => r.timings.duration < 500,
    });

    // We can also test the SSE stream, but standard k6 doesn't fully support SSE parsing without extensions.
    // So we just hit the root index.html to stress test the main HTTP pipeline.

    // Slight sleep to prevent purely unbounded local loop (simulate real user)
    sleep(0.1);
}
