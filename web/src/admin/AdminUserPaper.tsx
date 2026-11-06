// Admin: one user's paper trading, read-only (Admin → Users → user → Paper trading). The
// gateway lets only admins read another user's account (rest.md "Paper trading").

import { useEffect, useState } from 'react';
import { Link, useParams } from 'react-router-dom';

import { api } from '../api/client';
import { Paper } from '../pages/Paper';

export function AdminUserPaper() {
  const id = Number(useParams().userId);
  const [name, setName] = useState<string | null>(null);
  useEffect(() => {
    api
      .adminUser(id)
      .then((d) => setName(d.user.username))
      .catch(() => setName(null));
  }, [id]);
  return (
    <Paper
      viewUser={id}
      title={
        <Link to={`/admin/users/${id}`} className="small">
          ← {name ?? 'User'}
        </Link>
      }
    />
  );
}
