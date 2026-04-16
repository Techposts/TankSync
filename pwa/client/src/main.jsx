// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import React from 'react';
import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import App from './App.jsx';
import './index.css';

// Auto-update: when a new service worker activates, reload immediately.
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.addEventListener('controllerchange', () => {
    window.location.reload();
  });
}

// Theme: restore saved preference or use system default
const savedTheme = localStorage.getItem('tanksync_theme');
if (savedTheme === 'light') {
  document.documentElement.classList.add('light');
} else if (!savedTheme && window.matchMedia('(prefers-color-scheme: light)').matches) {
  document.documentElement.classList.add('light');
}

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </React.StrictMode>
);
