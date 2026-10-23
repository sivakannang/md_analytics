// The Admin page's tabs: Users (ADR-0019) and System (docs/design/system-monitor.md).

import { NavLink } from 'react-router-dom';

export function AdminTabs() {
  return (
    <nav className="chart-intervals admin-tabs" aria-label="Admin">
      <NavLink to="/admin" end className={({ isActive }) => (isActive ? 'active' : '')}>
        Users
      </NavLink>
      <NavLink to="/admin/system" className={({ isActive }) => (isActive ? 'active' : '')}>
        System
      </NavLink>
    </nav>
  );
}
