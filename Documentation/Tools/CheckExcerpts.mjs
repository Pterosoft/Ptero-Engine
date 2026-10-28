// Checks Documentation/Manual/EXCERPTS.md against the rules at the top of that file: every
// manual page has an excerpt, each is plain text, and each is 20 to 35 words; section excerpts
// point at real sections, are 12 to 30 words, and cover every ## section of their chapter. Run it from the
// repository root after adding or renaming a manual page:
//
//   node Documentation/Tools/CheckExcerpts.mjs
//
// Exits with 1 and lists the problems when anything is wrong.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const manualDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'Manual');
const text = fs.readFileSync(path.join(manualDir, 'EXCERPTS.md'), 'utf8').replace(/\r/g, '');

// Top-level pages: | Order | `Source` | Title | Excerpt |. Subtopics add a Parent column:
// | Order | `Source` | Title | `Parent` | Excerpt |, ordered within their parent.
const excerpts = [...text.matchAll(/^\| (\d+) \| `([^`]+)` \| ([^|]+) \| ([^|]+) \|$/gm)]
    .map(([, order, source, , excerpt]) => ({ order: Number(order), source, excerpt, parent: '' }));
const root = text.match(/\*\*Source:\*\* `([^`]+)`[\s\S]*?\*\*Order:\*\* (\d+)[\s\S]*?\*\*Excerpt:\*\* (.+)/);
if (root) excerpts.unshift({ order: Number(root[2]), source: root[1], excerpt: root[3], parent: '' });
for (const [, order, source, , parent, excerpt] of text.matchAll(/^\| (\d+) \| `([^`]+)` \| ([^|]+) \| `([^`]+)` \| ([^|]+) \|$/gm))
    excerpts.push({ order: Number(order), source, excerpt, parent });

const problems = [];
for (const { source, excerpt } of excerpts) {
    const words = excerpt.trim().split(/\s+/).length;
    if (words < 20 || words > 35) problems.push(`${source}: ${words} words (keep it to 20-35).`);
    if (/[`*_\[\]#→<>]/.test(excerpt)) problems.push(`${source}: contains markup; the website prints excerpts as plain text.`);
    if (!fs.existsSync(path.join(manualDir, source))) problems.push(`${source}: no such page in Documentation/Manual.`);
}
// ---- section excerpts ---------------------------------------------------------------------
const anchorOf = heading => heading.toLowerCase().replace(/[^a-z0-9 -]/g, '').replace(/ /g, '-');
const sectionExcerpts = [...text.matchAll(/^\| `([^`#]+)#([a-z0-9-]+)` \| ([^|]+) \| (.+) \|$/gm)]
    .map(([, source, anchor, , excerpt]) => ({ source, anchor, excerpt }));
const chaptersWithSections = new Set(sectionExcerpts.map(s => s.source));
for (const { source, anchor, excerpt } of sectionExcerpts) {
    const where = `${source}#${anchor}`;
    const words = excerpt.trim().split(/\s+/).length;
    if (words < 12 || words > 30) problems.push(`${where}: ${words} words (keep section excerpts to 12-30).`);
    if (/[`*_\[\]#→<>]/.test(excerpt)) problems.push(`${where}: contains markup; excerpts are plain text.`);
    if (!fs.existsSync(path.join(manualDir, source))) { problems.push(`${where}: no such page.`); continue; }
    const headings = fs.readFileSync(path.join(manualDir, source), 'utf8').split('\n')
        .filter(line => /^##+ /.test(line)).map(line => anchorOf(line.replace(/^#+ /, '')));
    if (!headings.includes(anchor)) problems.push(`${where}: no such section.`);
}
for (const source of chaptersWithSections) {
    if (!fs.existsSync(path.join(manualDir, source))) continue;
    for (const line of fs.readFileSync(path.join(manualDir, source), 'utf8').split('\n').filter(l => /^## /.test(l))) {
        const anchor = anchorOf(line.slice(3));
        if (!sectionExcerpts.some(s => s.source === source && s.anchor === anchor))
            problems.push(`${source}#${anchor}: section has no excerpt.`);
    }
}

// Orders must be unique among pages with the same parent.
const orderKeys = excerpts.map(e => `${e.parent}|${e.order}`);
for (const key of new Set(orderKeys.filter((k, i) => orderKeys.indexOf(k) !== i))) {
    const [parent, order] = key.split('|');
    problems.push(`Order ${order} is used more than once${parent ? ` under ${parent}` : ''}.`);
}
for (const { source, parent } of excerpts)
    if (parent && !excerpts.some(e => e.source === parent && !e.parent))
        problems.push(`${source}: its parent ${parent} has no page excerpt.`);
for (const page of fs.readdirSync(manualDir).filter(f => f.endsWith('.md') && f !== 'EXCERPTS.md'))
    if (!excerpts.some(e => e.source === page)) problems.push(`${page}: has no excerpt.`);

if (problems.length) {
    console.error(problems.join('\n'));
    process.exit(1);
}
console.log(`${excerpts.length} page excerpts and ${sectionExcerpts.length} section excerpts, all within the rules.`);
