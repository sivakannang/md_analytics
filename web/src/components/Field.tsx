import type { InputHTMLAttributes } from 'react';

interface FieldProps extends InputHTMLAttributes<HTMLInputElement> {
  label: string;
  hint?: string;
}

/** Labelled input with an optional hint underneath. */
export function Field({ label, hint, id, ...input }: FieldProps) {
  const inputId = id ?? input.name;
  const hintId = hint ? `${inputId}-hint` : undefined;
  return (
    <div className="field">
      <label htmlFor={inputId}>{label}</label>
      <input id={inputId} aria-describedby={hintId} {...input} />
      {hint && (
        <small id={hintId} className="hint">
          {hint}
        </small>
      )}
    </div>
  );
}

export function FormError({ message }: { message: string | null }) {
  return message ? (
    <p className="form-error" role="alert">
      {message}
    </p>
  ) : null;
}
