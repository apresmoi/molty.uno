// Architecture and style rules for apps/website/src (see apps/website/AGENTS.md). No baselines, no
// exemptions: a violation fails `pnpm check`. Usage: node scripts/checks/architecture.mjs [srcDir]
import fs from 'node:fs';
import path from 'node:path';
import ts from 'typescript';
import postcss from 'postcss';
import { parse } from '@astrojs/compiler';

const root = process.argv[2] ? path.resolve(process.argv[2]) : path.resolve(import.meta.dirname, '../../src');
const walk = (dir) =>
  fs.readdirSync(dir, { withFileTypes: true }).flatMap((entry) => (entry.isDirectory() ? walk(path.join(dir, entry.name)) : [path.join(dir, entry.name)]));
const files = walk(root).filter((file) => !file.includes(`${path.sep}content${path.sep}docs${path.sep}`));
const errors = [];
const report = (file, message) => errors.push(`${path.relative(root, file) || '.'}: ${message}`);
const rel = (file) => path.relative(root, file).split(path.sep).join('/');
const resolve = (file, spec) => {
  if (!spec.startsWith('.')) return;
  const base = path.resolve(path.dirname(file), spec);
  return [base, ...['.ts', '.tsx', '/index.ts'].map((ext) => base + ext)].find((f) => fs.existsSync(f) && fs.statSync(f).isFile());
};
const owner = (file) => /^components\/([^/]+)/.exec(rel(file))?.[1];
const graph = new Map();

/** Every import edge: barrels between owners, no UI from lower layers. */
function checkImport(file, target) {
  const fromOwner = owner(file);
  const toOwner = owner(target);
  if (toOwner && toOwner !== fromOwner && path.basename(target) !== 'index.ts') report(file, `Import ${toOwner} through its public index.ts.`);
  if (toOwner && /^(features|services|hooks|utils|config)\//.test(rel(file))) report(file, 'Lower layers (features, services, hooks, utils, config) cannot depend on UI components.');
  const service = /^services\/([^/]+)\//.exec(rel(target))?.[1];
  if (service && !rel(file).startsWith(`services/${service}/`) && path.basename(target) !== 'index.ts') report(file, `Import services/${service} through its index.ts.`);
}

/** The creature engine is the repo's (device/creatures/js), imported via @creatures — never forked. */
function checkBareImport(file, spec) {
  if (spec.startsWith('@creatures/') && !rel(file).startsWith('services/molty-stage/') && !rel(file).endsWith('env.d.ts'))
    report(file, 'Only services/molty-stage may import the creature engine (@creatures/…).');
  if ((spec === 'glyphcss' || spec.startsWith('@glyphcss/')) && !rel(file).startsWith('services/molty-stage/'))
    report(file, 'Only services/molty-stage may import the fluid renderer (glyphcss, @glyphcss/…).');
  if (spec === 'three' || spec.startsWith('three/')) report(file, 'three.js loads from the CDN inside the engine; do not import it.');
}

if (fs.existsSync(path.join(root, 'lib'))) report(root, 'Use hooks, utils, services or a feature owner instead of lib.');
for (const dir of fs.readdirSync(path.join(root, 'components'))) {
  const folder = path.join(root, 'components', dir);
  if (!fs.statSync(folder).isDirectory()) {
    report(folder, 'Components belong in a named folder.');
    continue;
  }
  for (const filename of [`${dir}.tsx`, 'index.ts']) if (!fs.existsSync(path.join(folder, filename))) report(folder, `Missing ${filename}.`);
}

for (const file of files) {
  const relative = rel(file);
  const source = fs.readFileSync(file, 'utf8');

  if (/\.(js|mjs)$/.test(file) && !relative.startsWith('config/')) report(file, 'Source is TypeScript; plain .mjs is only for config/ shared with Node scripts.');
  if (/(^|\/)(sim|targets|render|fluid-stage|kit)\.js$/.test(relative)) report(file, 'Looks like a copy of the creature engine; import it via @creatures instead.');
  if (/github\.com\//.test(source) && relative !== 'config/site.mjs') report(file, 'GitHub URLs come from REPO_URL in config/site.mjs.');

  if (file.endsWith('.astro')) {
    if (!/^(pages|layouts|integrations\/(astro|starlight))\//.test(relative)) report(file, 'Astro files belong in pages/, layouts/ or integrations/; reusable UI is TSX.');
    const { ast } = await parse(source);
    const visit = (node) => {
      if (node.type === 'frontmatter') {
        const sf = ts.createSourceFile(file + '.ts', node.value, ts.ScriptTarget.Latest, true);
        for (const statement of sf.statements)
          if (ts.isImportDeclaration(statement) && ts.isStringLiteral(statement.moduleSpecifier)) {
            const spec = statement.moduleSpecifier.text;
            checkBareImport(file, spec);
            const target = resolve(file, spec);
            if (target) checkImport(file, target);
          }
      }
      if (node.type === 'element' && node.name === 'style') report(file, 'Styles belong to a component CSS Module.');
      if (node.type === 'element' && node.name === 'script' && relative.startsWith('pages/') && !node.attributes?.some((a) => a.name === 'type' && a.value === 'application/ld+json'))
        report(file, 'Page runtime belongs in a mounted component.');
      node.children?.forEach(visit);
    };
    visit(ast);
  }

  if (/\.tsx?$/.test(file)) {
    const sf = ts.createSourceFile(file, source, ts.ScriptTarget.Latest, true, file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS);
    const edges = [];
    const visitImport = (spec, typesOnly) => {
      checkBareImport(file, spec);
      const target = resolve(file, spec);
      if (!target) return;
      checkImport(file, target);
      if (!typesOnly && /\.tsx?$/.test(target)) edges.push(target);
    };
    for (const node of sf.statements) {
      if ((ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) && node.moduleSpecifier && ts.isStringLiteral(node.moduleSpecifier)) {
        const typesOnly = ts.isExportDeclaration(node)
          ? node.isTypeOnly
          : node.importClause?.isTypeOnly ||
            (node.importClause?.namedBindings && ts.isNamedImports(node.importClause.namedBindings) && !node.importClause.name && node.importClause.namedBindings.elements.every((s) => s.isTypeOnly));
        visitImport(node.moduleSpecifier.text, typesOnly);
      }
      if ((owner(file) || /^services\/[^/]+\/index\.ts$/.test(relative)) && path.basename(file) === 'index.ts' && (!ts.isExportDeclaration(node) || !node.exportClause || !ts.isNamedExports(node.exportClause)))
        report(file, 'Public barrels contain explicit named exports only.');
    }
    graph.set(file, edges);
    const visit = (node) => {
      if (ts.isCallExpression(node) && node.expression.kind === ts.SyntaxKind.ImportKeyword && node.arguments[0] && ts.isStringLiteral(node.arguments[0])) checkBareImport(file, node.arguments[0].text);
      if (ts.isJsxAttribute(node) && node.name.getText(sf) === 'style' && node.initializer && ts.isJsxExpression(node.initializer) && node.initializer.expression && ts.isObjectLiteralExpression(node.initializer.expression)) {
        for (const prop of node.initializer.expression.properties)
          if (ts.isPropertyAssignment(prop) && (ts.isStringLiteral(prop.initializer) || ts.isNumericLiteral(prop.initializer)))
            report(file, `Static inline ${prop.name.getText(sf)} belongs in CSS (line ${sf.getLineAndCharacterOfPosition(prop.pos).line + 1}).`);
      }
      ts.forEachChild(node, visit);
    };
    visit(sf);
  }

  if (file.endsWith('.css') && !file.endsWith('/tokens.css')) {
    const css = postcss.parse(source);
    if (owner(file) && !file.endsWith('.module.css')) report(file, 'Component styles must be CSS Modules.');
    css.walkDecls((decl) => {
      if (/#(?:[\da-f]{3,8})\b|rgba?\(\s*\d|hsla?\(/i.test(decl.value)) report(file, `Use a colour token for ${decl.prop} (line ${decl.source.start.line}).`);
      if ((decl.prop === 'font-family' || decl.prop === 'font') && /monospace|sans-serif|serif/.test(decl.value)) report(file, `Use a font token (line ${decl.source.start.line}).`);
    });
  }
}

const visited = new Set();
const active = new Set();
const stack = [];
function cycles(file) {
  if (active.has(file)) {
    report(file, `Runtime import cycle: ${stack.slice(stack.indexOf(file)).map(rel).join(' → ')}`);
    return;
  }
  if (visited.has(file)) return;
  visited.add(file);
  active.add(file);
  stack.push(file);
  graph.get(file)?.forEach(cycles);
  stack.pop();
  active.delete(file);
}
for (const file of graph.keys()) cycles(file);

if (errors.length) {
  console.error(errors.join('\n'));
  process.exitCode = 1;
} else console.log(`Website architecture and style checks passed (${files.length} files).`);
