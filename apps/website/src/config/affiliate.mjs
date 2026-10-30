// Shop links. Every Amazon link on the site goes through shopLink(), which adds the
// Associates tag below; nothing else spells the tag. The site owner fills AMAZON_TAG with
// the tracking ID from their Amazon Associates account (amazon.de, ends in "-21").
// Plain .mjs: the components and the Node unit tests import the same file.
export const AMAZON_TAG = 'molty-21';

/** The disclosure Amazon requires wherever tagged links appear. */
export const AMAZON_DISCLOSURE = 'As an Amazon Associate we earn from qualifying purchases.';

/** An amazon.de search: a product search never goes dead the way a product page does.
 *  @param {string} query */
export const amazonSearch = (query) => `https://www.amazon.de/s?k=${encodeURIComponent(query).replace(/%20/g, '+')}`;

const AMAZON_HOST = /(^|\.)amazon\.[a-z.]{2,6}$/i;

/** Any outbound shop URL → its href and rel. Amazon URLs get the tag (replacing any other)
 *  and rel="sponsored"; everything else is a plain external link.
 *  @param {string} url
 *  @returns {{ href: string, rel: string, sponsored: boolean }} */
export function shopLink(url) {
  const u = new URL(url);
  if (u.protocol !== 'https:') throw new Error(`shop links must be https: ${url}`);
  if (!AMAZON_HOST.test(u.hostname)) return { href: u.href, rel: 'noopener noreferrer', sponsored: false };
  u.searchParams.set('tag', AMAZON_TAG);
  return { href: u.href, rel: 'sponsored noopener', sponsored: true };
}
