// Markdown imported as text, via the [[rules]] Text entry in wrangler.toml.
declare module "*.md" {
    const content: string;
    export default content;
}
