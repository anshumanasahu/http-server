const { test, expect } = require('@playwright/test');

test.describe('Multi-Protocol Dashboard', () => {
  test.beforeEach(async ({ page }) => {
    // Navigate to the dashboard before each test
    await page.goto('http://localhost:8080');
  });

  test('should load the dashboard correctly', async ({ page }) => {
    // Verify the title
    await expect(page).toHaveTitle(/HTTP Server/);
    
    // Verify headers exist
    await expect(page.locator('h1')).toHaveText(/C\+\+ Backend Architecture Showcase/);
    
    // Verify all 4 protocol cards are present
    await expect(page.locator('#proto-http')).toBeVisible();
    await expect(page.locator('#proto-ftp')).toBeVisible();
    await expect(page.locator('#proto-smtp')).toBeVisible();
    await expect(page.locator('#proto-imap')).toBeVisible();
  });

  test('should have chaos sandbox buttons', async ({ page }) => {
    // Verify the buttons exist
    const burstBtn = page.locator('button', { hasText: 'Sim Traffic Burst' });
    const slowlorisBtn = page.locator('button', { hasText: 'Sim Slowloris' });
    
    await expect(burstBtn).toBeVisible();
    await expect(slowlorisBtn).toBeVisible();
  });
});
