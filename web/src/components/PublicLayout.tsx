import { Link, NavLink, Outlet } from 'react-router-dom';

/** Shell for signed-out pages (home, sign in, sign up): brand plus account links. */
export function PublicLayout() {
  return (
    <div className="shell">
      <header className="topbar">
        <Link to="/" className="brand">
          md_analytics
        </Link>
        <div className="spacer" />
        <nav className="public-nav">
          <NavLink to="/signin">Sign in</NavLink>
          <NavLink to="/signup" className="button-link">
            Sign up
          </NavLink>
        </nav>
      </header>
      <Outlet />
    </div>
  );
}
