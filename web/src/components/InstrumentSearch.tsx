// Search box with a results dropdown. Typing searches the gateway (debounced); arrow keys
// move through the results, Enter adds the highlighted one, Escape closes the list.

import { useEffect, useId, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';

import { api, errorMessage } from '../api/client';
import type { Instrument } from '../api/client';
import { instrumentDetail, instrumentTitle } from '../watchlists/format';

const DEBOUNCE_MS = 200;

interface Props {
  /** Instruments already in the current list: shown as added, not addable again. */
  added: ReadonlySet<number>;
  onAdd: (instrument: Instrument) => void;
  disabled?: boolean;
}

export function InstrumentSearch({ added, onAdd, disabled }: Props) {
  const [query, setQuery] = useState('');
  const [results, setResults] = useState<Instrument[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(0);
  const listId = useId();
  const boxRef = useRef<HTMLDivElement>(null);

  // Search after a pause in typing. A newer query cancels the older one's result.
  useEffect(() => {
    const text = query.trim();
    if (text === '') {
      setResults([]);
      setError(null);
      return;
    }
    let stale = false;
    const timer = setTimeout(() => {
      api
        .searchInstruments(text)
        .then((found) => {
          if (!stale) {
            setResults(found);
            setError(null);
            setActive(0);
          }
        })
        .catch((e) => !stale && setError(errorMessage(e)));
    }, DEBOUNCE_MS);
    return () => {
      stale = true;
      clearTimeout(timer);
    };
  }, [query]);

  // Close when clicking anywhere outside the box.
  useEffect(() => {
    const close = (event: MouseEvent) => {
      if (!boxRef.current?.contains(event.target as Node)) {
        setOpen(false);
      }
    };
    document.addEventListener('mousedown', close);
    return () => document.removeEventListener('mousedown', close);
  }, []);

  const choose = (instrument: Instrument | undefined) => {
    if (instrument && !added.has(instrument.instrument_id)) {
      onAdd(instrument);
    }
  };

  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      setOpen(true);
      setActive((n) => Math.min(n + 1, results.length - 1));
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      setActive((n) => Math.max(n - 1, 0));
    } else if (event.key === 'Enter') {
      event.preventDefault();
      choose(results[active]);
    } else if (event.key === 'Escape') {
      setOpen(false);
    }
  };

  const showList = open && query.trim() !== '';

  return (
    <div className="search" ref={boxRef}>
      <input
        type="search"
        placeholder="Search eg: infy, nifty 25000 ce, banknifty fut"
        aria-label="Search instruments"
        role="combobox"
        aria-expanded={showList}
        aria-controls={listId}
        aria-activedescendant={showList && results[active] ? `${listId}-${active}` : undefined}
        autoComplete="off"
        spellCheck={false}
        disabled={disabled}
        value={query}
        onChange={(e) => {
          setQuery(e.target.value);
          setOpen(true);
        }}
        onFocus={() => setOpen(true)}
        onKeyDown={onKeyDown}
      />
      {showList && (
        <ul className="search-results" id={listId} role="listbox">
          {error && <li className="search-empty">{error}</li>}
          {!error && results.length === 0 && <li className="search-empty">No matches</li>}
          {results.map((instrument, n) => {
            const isAdded = added.has(instrument.instrument_id);
            return (
              <li
                key={instrument.instrument_id}
                id={`${listId}-${n}`}
                role="option"
                aria-selected={n === active}
                className={n === active ? 'active' : undefined}
                onMouseEnter={() => setActive(n)}
                onMouseDown={(e) => e.preventDefault()} // keep focus in the input
                onClick={() => choose(instrument)}
              >
                <span className="instrument">
                  <span className="title">{instrumentTitle(instrument)}</span>
                  <span className="detail">{instrumentDetail(instrument)}</span>
                </span>
                <span className="exchange">{instrument.exchange}</span>
                <span className={isAdded ? 'add added' : 'add'} aria-hidden>
                  {isAdded ? '✓' : '+'}
                </span>
              </li>
            );
          })}
        </ul>
      )}
    </div>
  );
}
