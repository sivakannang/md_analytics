import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom';

import { AdminUserDetail } from './admin/AdminUserDetail';
import { AdminSystem } from './admin/AdminSystem';
import { AdminUsers } from './admin/AdminUsers';
import { RequireAdmin } from './admin/RequireAdmin';
import { AuthProvider, useAuth } from './auth/AuthContext';
import { Layout } from './components/Layout';
import { PublicLayout } from './components/PublicLayout';
import { RequireAuth } from './components/RequireAuth';
import { Account } from './pages/Account';
import { Dashboard } from './pages/Dashboard';
import { InstrumentPage } from './pages/InstrumentPage';
import { OptionChain } from './pages/OptionChain';
import { Landing } from './pages/Landing';
import { SignIn } from './pages/SignIn';
import { SignUp } from './pages/SignUp';
import './styles.css';

/** "/" is the public home page for visitors and the dashboard once signed in. */
function RootShell() {
  const { state } = useAuth();
  if (state.status === 'loading') {
    return <p className="loading">Loading…</p>;
  }
  return state.status === 'signed-in' ? <Layout /> : <PublicLayout />;
}

function RootIndex() {
  const { state } = useAuth();
  return state.status === 'signed-in' ? <Dashboard /> : <Landing />;
}

const root = document.getElementById('root');
if (root === null) {
  throw new Error('missing #root element');
}

createRoot(root).render(
  <StrictMode>
    <BrowserRouter>
      <AuthProvider>
        <Routes>
          <Route element={<PublicLayout />}>
            <Route path="/signin" element={<SignIn />} />
            <Route path="/signup" element={<SignUp />} />
          </Route>
          <Route path="/" element={<RootShell />}>
            <Route index element={<RootIndex />} />
            <Route
              path="instrument/:instrumentId"
              element={
                <RequireAuth>
                  <InstrumentPage />
                </RequireAuth>
              }
            />
            <Route
              path="chain"
              element={
                <RequireAuth>
                  <OptionChain />
                </RequireAuth>
              }
            />
            <Route
              path="admin"
              element={
                <RequireAdmin>
                  <AdminUsers />
                </RequireAdmin>
              }
            />
            <Route
              path="admin/system"
              element={
                <RequireAdmin>
                  <AdminSystem />
                </RequireAdmin>
              }
            />
            <Route
              path="admin/users/:userId"
              element={
                <RequireAdmin>
                  <AdminUserDetail />
                </RequireAdmin>
              }
            />
            <Route
              path="account"
              element={
                <RequireAuth>
                  <Account />
                </RequireAuth>
              }
            />
          </Route>
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </AuthProvider>
    </BrowserRouter>
  </StrictMode>,
);
