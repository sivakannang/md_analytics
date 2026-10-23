import { Link } from 'react-router-dom';

const FEATURES = [
  {
    title: 'Live option chain',
    text: 'NIFTY and stock F&O strikes with price, volume and open interest, updating every second.',
  },
  {
    title: 'OI buildup at a glance',
    text: 'Long buildup, short buildup, short covering and long unwinding — colour-coded per candle.',
  },
  {
    title: 'Price and OI charts',
    text: 'Candles across timeframes, built from recorded market data, with history for comparison.',
  },
  {
    title: 'Your watchlists',
    text: 'Track the instruments you care about; saved on the server and available everywhere.',
  },
];

/** Public home page for visitors who are not signed in. */
export function Landing() {
  return (
    <main className="landing">
      <section className="hero">
        <h1>Market data analytics for Indian derivatives</h1>
        <p className="lead">
          Real-time option chains, open-interest analytics and charts for NIFTY and NSE F&amp;O
          stocks.
        </p>
        <div className="cta">
          <Link to="/signup" className="button-link large">
            Create an account
          </Link>
          <Link to="/signin" className="button-link large secondary">
            Sign in
          </Link>
        </div>
      </section>

      <section className="features" aria-label="Features">
        {FEATURES.map((feature) => (
          <article key={feature.title} className="feature">
            <h2>{feature.title}</h2>
            <p>{feature.text}</p>
          </article>
        ))}
      </section>

      <footer className="footer">md_analytics · personal research project · not investment advice</footer>
    </main>
  );
}
