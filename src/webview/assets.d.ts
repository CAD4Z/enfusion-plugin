/** esbuild turns a stylesheet import into the bundle's own `.css`; TypeScript only needs to know it resolves. */
declare module '*.css';
